/* Host execve entry and execution address-space lifecycle. */

#define pr_fmt(fmt) "flux_mm: " fmt

#include "internal.h"

/**
 * flux_execve() - execute the host execve path for a Flux request
 * @arg: userspace pointer to struct flux_execve_args
 *
 * Return: the host execve result or a negative errno.
 */
int flux_execve(unsigned long arg)
{
	struct pt_regs regs;
	struct flux_execve_args args;
	long ret;
	bool map_shared = test_thread_flag(TIF_HOOK_ACTIVE);

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	pr_debug("execve: filename=%lx, argv=%lx, envp=%lx\n",
		 (unsigned long)args.filename, (unsigned long)args.argv,
		 (unsigned long)args.envp);

	/* execve will not use the temporary regs */
	regs = *current_pt_regs();
	regs.di = (unsigned long)args.filename;
	regs.si = (unsigned long)args.argv;
	regs.dx = (unsigned long)args.envp;

	ret = flux_sys_execve(&regs);
	if (map_shared) {
		flux_remap_stack();
	}
	return ret;
}

/**
 * flux_copy_mm() - prepare a fork slot or duplicate the exec template
 * @ctx: MM context which will own the new slot
 * @proc_key_ptr: userspace destination for the allocated process key
 * @for_fork: true for Flux fork, false for Flux exec construction
 *
 * Fork acquires the caller's alias gate before LibOS COW, but defers host
 * duplication until flux_fork_alias_begin(), after LibOS dup_mmap succeeds.
 * Exec copies the runtime template recorded before the first application
 * is loaded; it never inherits application mappings or a fork gate.
 *
 * Return: 0 on success, or a negative errno.
 */
int flux_copy_mm(struct flux_mm_ctx *ctx, int __user *proc_key_ptr,
		 bool for_fork)
{
	struct mm_struct *oldmm = current->mm;
	struct mm_struct *mm;
	struct flux_proc *proc;
	u32 proc_key;
	int err;

	if (!oldmm)
		return -EINVAL;
	proc = flux_proc_alloc(GFP_KERNEL);
	if (!proc)
		return -ENOMEM;
	flux_proc_init(proc, 1, NULL);
	proc->for_fork = for_fork;

	if (for_fork) {
		/* Linux owns COW; gate only the projections involved in that fork. */
		err = flux_alias_gate_acquire_mm(ctx, oldmm);
		if (err)
			goto out;
		proc->alias_gate_mm = oldmm;
	} else {
		mutex_lock(&ctx->lock);
		oldmm = ctx->exec_mm;
		if (!oldmm) {
			mutex_unlock(&ctx->lock);
			err = -ENOENT;
			goto out;
		}
		mm = flux_dup_mm(current, oldmm);
		mutex_unlock(&ctx->lock);
		if (!mm) {
			err = -ENOMEM;
			goto out;
		}
		proc->mm = mm;
	}

	/*
	 * A reserved xarray ID has no visible proc. Complete copyout and state
	 * initialization before the one publication point; all earlier failures
	 * belong exclusively to this constructor, without RCU or active refs.
	 */
	err = xa_alloc_cyclic(&ctx->procs, &proc_key, NULL,
			      flux_proc_xa_limit, &ctx->next_proc_key, GFP_KERNEL);
	if (err < 0)
		goto out;
	proc->key = proc_key;
	if (copy_to_user(proc_key_ptr, &proc_key, sizeof(proc_key))) {
		err = -EFAULT;
		goto out_key;
	}
	if (for_fork)
		atomic_set(&proc->alias_state, FLUX_ALIAS_STATE_PENDING);
	err = xa_err(xa_store(&ctx->procs, proc_key, proc, GFP_KERNEL));
	if (err)
		goto out_key;

	/* Fork owns only its gate until LibOS accepts the new address space. */
	return 0;

out_key:
	xa_erase(&ctx->procs, proc_key);
out:
	if (proc->mm) {
		flux_alias_release_mm(ctx, proc->mm);
		mmput(proc->mm);
	}
	if (proc->alias_gate_mm)
		flux_alias_gate_release(ctx, proc->alias_gate_mm);
	flux_proc_free(proc);
	return err;
}

/**
 * flux_release_mm() - release process ownership of a duplicated host mm
 * @ctx: MM context containing the process slot
 * @proc_key: nonzero key of the slot to release
 *
 * The slot is hidden from new lookups before its ownership reference is
 * dropped.  Existing active references keep the slot and host mm alive until
 * their final flux_proc_put().
 *
 * Return: 0 on success, or a negative errno.
 */
int flux_release_mm(struct flux_mm_ctx *ctx, int proc_key)
{
	struct mm_struct *gate_mm;
	struct flux_proc *proc;
	int alias_state;
	int ret;

	pr_debug("release mm for proc_key=%d\n", proc_key);

	if (proc_key <= 0)
		return -EINVAL;

	xa_lock(&ctx->procs);
	proc = xa_load(&ctx->procs, proc_key);
	if (!proc || READ_ONCE(proc->released)) {
		proc = NULL;
		ret = -ENOENT;
	} else {
		/* Publish release and transfer gate ownership as one transition. */
		WRITE_ONCE(proc->released, true);
		alias_state = atomic_xchg(&proc->alias_state, FLUX_ALIAS_STATE_NONE);
		gate_mm = xchg(&proc->alias_gate_mm, NULL);
		ret = 0;
	}
	xa_unlock(&ctx->procs);

	if (!proc)
		return -ENOENT;

	if (gate_mm)
		ret = flux_alias_gate_release(ctx, gate_mm);
	else if (WARN_ON_ONCE(alias_state != FLUX_ALIAS_STATE_NONE))
		ret = -EINVAL;

	/* Drop exactly one process ownership reference. */
	flux_proc_put(ctx, proc_key, proc);

	return ret;
}

/**
 * flux_switch_mm() - switch current between Flux host address spaces
 * @ctx: MM context containing both process slots
 * @proc_key_to: key of the target process slot
 * @proc_key_from: key recorded as active on this Flux CPU
 *
 * The calling worker must remain bound to its Flux CPU.  The source uses the
 * per-CPU active reference; the target lookup reference becomes the new
 * active reference after switch_mm_irqs_off().
 *
 * Return: 0 on success, %-ENOENT for a missing target, or %-EINVAL when the
 * supplied source does not match current's active mm state.
 */
int flux_switch_mm(struct flux_mm_ctx *ctx, int proc_key_to, int proc_key_from)
{
	struct flux_proc *proc_to = NULL, *proc_from = NULL;
	struct mm_struct *mm_to = NULL, *mm_from = NULL;
	struct task_struct *p = current;
	struct flux_percpu *pcpu;
	int err = 0;

	if (proc_key_to < 0 || proc_key_from < 0 ||
	    proc_key_to == proc_key_from)
		return -EINVAL;

	pcpu = get_cpu_ptr(&flux_percpu);
	if (pcpu->active_mm_ctx == ctx && pcpu->active_mm_task == p) {
		proc_from = pcpu->active_proc;
		if (!proc_from || proc_from->key != proc_key_from) {
			err = -EINVAL;
			goto out_cpu;
		}
	} else {
		/* proc0 starts with one active reference for every Flux CPU. */
		if ((pcpu->active_mm_ctx && pcpu->active_mm_task == p) ||
		    proc_key_from != 0 || ctx->proc0.mm != p->mm ||
		    ctx->proc0.mm != p->active_mm) {
			err = -EINVAL;
			goto out_cpu;
		}
		proc_from = &ctx->proc0;
	}

	proc_to = flux_proc_get_by_key(ctx, proc_key_to);
	if (!proc_to) {
		err = -ENOENT;
		goto out_put;
	}

	mm_to = proc_to->mm;
	mm_from = proc_from->mm;
	if (!mm_to || !mm_from) {
		err = -ENOENT;
		goto out_put;
	}

	if (unlikely(mm_from != p->mm || mm_from != p->active_mm)) {
		err = -EINVAL;
		goto out_put;
	}

	mmget(mm_to);

#ifdef CONFIG_SCHED_MM_CID
	/*
	 * Flux switches current->mm by hand and runs the host runtime with rseq
	 * disabled. Drop the old scheduler MM-CID, but do not allocate a new one
	 * for these synthetic switches: sched_mm_cid_after_execve() can spin in
	 * mm_cid_get() under rapid Flux multiproc clone/exit workloads.
	 */
	if (READ_ONCE(p->mm_cid_active))
		flux_sched_mm_cid_before_execve(p);
#endif
	task_lock(p);
	local_irq_disable();
	flux_proc_save_stats(proc_from, p);
	flux_proc_restore_stats(proc_to, p);
	p->mm = p->active_mm = mm_to;
#ifdef CONFIG_MEMBARRIER
	flux_membarrier_update_current_mm(mm_to);
#endif
	flux_switch_mm_irqs_off(mm_from, mm_to, p);
	lru_gen_use_mm(mm_to);
	local_irq_enable();
	task_unlock(p);
	/* The target lookup reference becomes this CPU's active reference. */
	pcpu->active_mm_ctx = ctx;
	pcpu->active_proc = proc_to;
	pcpu->active_mm_task = p;
	put_cpu_ptr(&flux_percpu);
	/* Drop the task MM ref first; proc_from still owns the address space. */
	mmput(mm_from);
	flux_proc_put(ctx, proc_key_from, proc_from);
	return 0;

out_put:
	if (proc_to)
		flux_proc_put(ctx, proc_key_to, proc_to);
out_cpu:
	put_cpu_ptr(&flux_percpu);
	return err;
}
