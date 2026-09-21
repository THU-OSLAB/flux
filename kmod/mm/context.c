/* Context registry and Flux process-slot lifetime. */

#define pr_fmt(fmt) "flux_mm: " fmt

#include "fork_stats.h"
#include "internal.h"

LIST_HEAD(flux_mm_ctx_registry);
DEFINE_MUTEX(flux_mm_ctx_registry_lock);

static struct flux_mm_ctx *flux_mm_ctx_find_locked(pid_t tgid)
{
	struct flux_mm_ctx *ctx;

	list_for_each_entry(ctx, &flux_mm_ctx_registry, registry_node) {
		if (ctx->owner_tgid == tgid)
			return ctx;
	}

	return NULL;
}

static int flux_mm_ctx_register(struct flux_mm_ctx *ctx)
{
	int err = 0;

	mutex_lock(&flux_mm_ctx_registry_lock);
	if (flux_mm_ctx_find_locked(ctx->owner_tgid)) {
		err = -EEXIST;
	} else {
		list_add_tail_rcu(&ctx->registry_node, &flux_mm_ctx_registry);
	}
	mutex_unlock(&flux_mm_ctx_registry_lock);

	return err;
}

static void flux_mm_ctx_unregister(struct flux_mm_ctx *ctx)
{
	bool removed = false;

	mutex_lock(&flux_mm_ctx_registry_lock);
	if (!list_empty(&ctx->registry_node)) {
		list_del_rcu(&ctx->registry_node);
		removed = true;
	}
	mutex_unlock(&flux_mm_ctx_registry_lock);
	if (removed) {
		synchronize_rcu();
		INIT_LIST_HEAD(&ctx->registry_node);
	}
}

/**
 * flux_mm_mpk_enabled_current_rcu() - query MPK state for current's group
 *
 * Walk the context registry under RCU without taking a context reference.
 * This helper is suitable for hook paths which cannot acquire the registry
 * mutex; the returned value is only a snapshot.
 *
 * Return: %true when the current thread group's Flux context has MPK enabled.
 */
bool notrace flux_mm_mpk_enabled_current_rcu(void)
{
	struct flux_mm_ctx *ctx;
	pid_t tgid = task_tgid_nr(current);
	bool enabled = false;

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &flux_mm_ctx_registry, registry_node) {
		if (READ_ONCE(ctx->owner_tgid) == tgid) {
			enabled = READ_ONCE(ctx->mpk_enabled);
			break;
		}
	}
	rcu_read_unlock();
	return enabled;
}

/**
 * flux_mm_ctx_get_current() - get current's registered Flux MM context
 *
 * Return: a referenced context for the current thread group, or %NULL when no
 * context is registered.  The caller must release a successful result with
 * flux_mm_ctx_put().
 */
struct flux_mm_ctx *flux_mm_ctx_get_current(void)
{
	struct flux_mm_ctx *ctx = NULL;
	struct pid *leader_pid;

	leader_pid = get_task_pid(current->group_leader, PIDTYPE_PID);
	if (!leader_pid)
		return NULL;

	mutex_lock(&flux_mm_ctx_registry_lock);
	ctx = flux_mm_ctx_find_locked(task_tgid_nr(current));
	if (ctx && !kref_get_unless_zero(&ctx->refcount))
		ctx = NULL;
	mutex_unlock(&flux_mm_ctx_registry_lock);
	put_pid(leader_pid);

	return ctx;
}

/* Fault hooks cannot acquire the registry mutex before enabling interrupts. */
struct flux_mm_ctx *flux_mm_ctx_get_current_rcu(void)
{
	struct flux_mm_ctx *ctx, *found = NULL;
	pid_t tgid = task_tgid_nr(current);

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &flux_mm_ctx_registry, registry_node) {
		if (READ_ONCE(ctx->owner_tgid) == tgid &&
		    kref_get_unless_zero(&ctx->refcount)) {
			found = ctx;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

/**
 * flux_mm_ctx_put() - release a Flux MM context reference
 * @ctx: context returned by flux_mm_ctx_get_current(), or %NULL
 */
void flux_mm_ctx_put(struct flux_mm_ctx *ctx)
{
	if (ctx)
		kref_put(&ctx->refcount, flux_mm_ctx_release);
}

void flux_mm_ctx_reset_ranges(struct flux_mm_ctx *ctx)
{
	flux_mm_ranges_clear(&ctx->base_maps);
	if (ctx->exec_mm) {
		flux_alias_release_mm(ctx, ctx->exec_mm);
		mmput(ctx->exec_mm);
		ctx->exec_mm = NULL;
	}
}
static void flux_proc_reset_stats(struct flux_proc *proc)
{
	proc->min_flt = 0;
	proc->maj_flt = 0;
	proc->nvcsw = 0;
	proc->nivcsw = 0;
}

void flux_proc_save_stats(struct flux_proc *proc,
				 const struct task_struct *task)
{
	proc->min_flt = task->min_flt;
	proc->maj_flt = task->maj_flt;
	proc->nvcsw = task->nvcsw;
	proc->nivcsw = task->nivcsw;
}

void flux_proc_restore_stats(const struct flux_proc *proc,
				    struct task_struct *task)
{
	task->min_flt = proc->min_flt;
	task->maj_flt = proc->maj_flt;
	task->nvcsw = proc->nvcsw;
	task->nivcsw = proc->nivcsw;
}

/**
 * flux_proc_init() - initialize a Flux process slot
 * @proc: zeroed process slot to initialize
 * @count: initial ownership and active-reference count
 * @mm: host address space owned by the slot, or %NULL while being built
 */
void flux_proc_init(struct flux_proc *proc, int count,
			   struct mm_struct *mm)
{
	proc->key = 0;
	refcount_set(&proc->refs, count);
	proc->released = false;
	proc->for_fork = false;
	proc->fork_parent = NULL;
	proc->mm = mm;
	atomic_set(&proc->alias_state, FLUX_ALIAS_STATE_NONE);
	proc->alias_gate_mm = NULL;
	flux_proc_reset_stats(proc);
}

/*
 * Tear down a proc whose last reference is gone and which is no longer
 * reachable through the xarray.  The alias gate and mmput paths sleep,
 * so this must run without holding xa_lock.
 */
static void flux_proc_free_rcu(struct rcu_head *rcu)
{
	struct flux_proc *proc = container_of(rcu, struct flux_proc, rcu);

	flux_proc_free(proc);
}

/**
 * flux_proc_release() - destroy an unreachable process slot
 * @ctx: MM context which owned the slot
 * @proc_key: nonzero xarray key of the slot
 * @proc: slot whose reference count reached zero
 *
 * The caller must remove @proc from @ctx before this function is invoked.
 * This function may sleep while releasing the alias gate or host mm.  The
 * slab object itself is freed after an RCU grace period.
 */
void flux_proc_release(struct flux_mm_ctx *ctx, int proc_key,
			      struct flux_proc *proc)
{
	struct mm_struct *gate_mm;
	int alias_state;

	if (WARN_ON_ONCE(proc_key == 0))
		return;
	alias_state = atomic_xchg(&proc->alias_state,
				 FLUX_ALIAS_STATE_NONE);
	gate_mm = xchg(&proc->alias_gate_mm, NULL);
	if (gate_mm)
		flux_alias_gate_release(ctx, gate_mm);
	else
		WARN_ON_ONCE(alias_state != FLUX_ALIAS_STATE_NONE);
	if (proc->mm) {
		u64 start;
		bool saved;

		flux_alias_release_mm(ctx, proc->mm);
		start = flux_fork_stats_start();
		saved = flux_fork_mm_put(ctx, proc);
		flux_fork_stats_end(FLUX_FORK_STAT_CACHE_PUT, start);
		if (!saved) {
			start = flux_fork_stats_start();
			mmput(proc->mm);
			flux_fork_stats_end(FLUX_FORK_STAT_HOST_MMPUT, start);
		}
	}
	if (proc->fork_parent)
		mmdrop(proc->fork_parent);
	proc->mm = NULL;
	call_rcu(&proc->rcu, flux_proc_free_rcu);
}

/**
 * flux_proc_put() - release a process-slot reference
 * @ctx: MM context containing @proc
 * @proc_key: xarray key used to locate @proc
 * @proc: referenced process slot
 *
 * The last put removes the slot from the xarray and may sleep while releasing
 * its alias gate and host mm.  Callers must not invoke this from atomic
 * context.
 */
void flux_proc_put(struct flux_mm_ctx *ctx, int proc_key,
		   struct flux_proc *proc)
{
	if (!refcount_dec_and_test(&proc->refs))
		return;

	xa_erase(&ctx->procs, proc_key);
	flux_proc_release(ctx, proc_key, proc);
}

static void flux_mm_clear_active_ctx(struct flux_mm_ctx *ctx)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct flux_percpu *pcpu = per_cpu_ptr(&flux_percpu, cpu);
		struct flux_proc *proc;

		if (READ_ONCE(pcpu->active_mm_ctx) == ctx) {
			proc = READ_ONCE(pcpu->active_proc);
			WRITE_ONCE(pcpu->active_mm_task, NULL);
			WRITE_ONCE(pcpu->active_proc, NULL);
			WRITE_ONCE(pcpu->active_mm_ctx, NULL);
			if (proc && proc->key > 0)
				flux_proc_put(ctx, proc->key, proc);
		}
	}
}

static struct flux_proc *__flux_proc_get_by_key(struct flux_mm_ctx *ctx,
						 int proc_key)
{
	struct flux_proc *proc;

	rcu_read_lock();
	proc = xa_load(&ctx->procs, proc_key);
	if (proc && !refcount_inc_not_zero(&proc->refs))
		proc = NULL;
	rcu_read_unlock();
	if (proc && READ_ONCE(proc->released)) {
		flux_proc_put(ctx, proc_key, proc);
		proc = NULL;
	}

	return proc;
}

/**
 * flux_proc_get_by_key() - look up a live process slot
 * @ctx: MM context containing the process registry
 * @proc_key: xarray key of the requested process slot
 *
 * The lookup is RCU protected and acquires a reference with
 * refcount_inc_not_zero().  Released slots are not returned.
 *
 * Return: a referenced process slot, or %NULL.  A successful lookup must be
 * paired with flux_proc_put().
 */
struct flux_proc *flux_proc_get_by_key(struct flux_mm_ctx *ctx,
					      int proc_key)
{
	return __flux_proc_get_by_key(ctx, proc_key);
}

static struct flux_mm_ctx *flux_mm_ctx_alloc(void)
{
	struct flux_mm_ctx *ctx;
	struct mm_struct *mm = current->mm;
	int err;

	if (unlikely(!mm))
		return NULL;

	ctx = kzalloc(sizeof(struct flux_mm_ctx), GFP_KERNEL);
	if (unlikely(!ctx))
		return NULL;

	/*
	 * proc0 persists after the host task switches away from the original mm,
	 * so it needs its own lifetime ref just like duplicated mms do.
	 */
	mmget(mm);

	xa_init_flags(&ctx->procs, XA_FLAGS_ALLOC);
	ctx->next_proc_key = 1;
	flux_proc_init(&ctx->proc0, 1 + flux_shm->used_cpus, mm);
	xa_store(&ctx->procs, 0, &ctx->proc0, GFP_KERNEL);

	kref_init(&ctx->refcount);
	mutex_init(&ctx->lock);
	spin_lock_init(&ctx->fork_cache_lock);
	ctx->owner_tgid = task_tgid_nr(current);
	INIT_LIST_HEAD(&ctx->base_maps);
	for (unsigned int bucket = 0; bucket < FLUX_ALIAS_LOCK_COUNT; bucket++)
		INIT_LIST_HEAD(&ctx->alias_mms[bucket]);
	xa_init(&ctx->alias_sources);
	INIT_LIST_HEAD(&ctx->registry_node);

	err = flux_mm_ctx_register(ctx);
	if (unlikely(err)) {
		mmput(mm);
		kfree(ctx);
		return NULL;
	}

	return ctx;
}

/**
 * flux_mm_ctx_release() - kref release callback for a Flux MM context
 * @ref: final reference embedded in struct flux_mm_ctx
 *
 * Removes the context from all registries, drops process ownership
 * references, and tears down its cached mappings.
 */
void flux_mm_ctx_release(struct kref *ref)
{
	struct flux_mm_ctx *ctx =
		container_of(ref, struct flux_mm_ctx, refcount);
	struct flux_proc *proc;
	unsigned long proc_key;

	flux_fork_mm_drain(ctx);
	flux_mpk_ctx_release(ctx);
	flux_mm_clear_active_ctx(ctx);

	xa_for_each_start(&ctx->procs, proc_key, proc, 1) {
		xa_erase(&ctx->procs, proc_key);
		flux_proc_put(ctx, proc_key, proc);
	}

	flux_mm_ctx_unregister(ctx);
	mutex_lock(&ctx->lock);
	flux_mm_ctx_reset_ranges(ctx);
	mutex_unlock(&ctx->lock);

	if (ctx->proc0.mm) {
		flux_alias_release_mm(ctx, ctx->proc0.mm);
		mmput(ctx->proc0.mm);
	}

	WARN_ON_ONCE(!xa_empty(&ctx->alias_sources));
	xa_destroy(&ctx->alias_sources);
	kfree(ctx);
}

/**
 * flux_mm_setup_ctx() - allocate an MM context for a device file
 * @filp: newly opened Flux MM device file
 *
 * Return: 0 on success, or a negative errno.  On success @filp owns the
 * initial context reference through filp->private_data.
 */
int flux_mm_setup_ctx(struct file *filp)
{
	struct flux_mm_ctx *ctx;
	struct flux_percpu *pcpu;

	if (filp->private_data)
		return -EINVAL;

	ctx = flux_mm_ctx_alloc();
	if (!ctx)
		return -ENOMEM;

	pcpu = get_cpu_ptr(&flux_percpu);
	if (!pcpu->active_mm_ctx || pcpu->active_mm_task != current) {
		pcpu->active_mm_ctx = ctx;
		pcpu->active_proc = &ctx->proc0;
		pcpu->active_mm_task = current;
	}
	put_cpu_ptr(&flux_percpu);

	filp->private_data = ctx;
	return 0;
}
