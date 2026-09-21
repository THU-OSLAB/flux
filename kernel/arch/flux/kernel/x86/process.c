#define pr_fmt(fmt) "process: " fmt

#include <linux/entry-common.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/module.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/prctl.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/uaccess.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/host_dev.h>
#include <asm/current.h>
#include <asm/mmu.h>
#include <asm/smp.h>
#include <asm/x86/ptrace.h>
#include <asm/x86/switch_to.h>
#include <asm/x86/fpu.h>
#include <asm/x86/uintr.h>
#include <asm/x86/cpuid.h>
#include <asm/x86/processor.h>
#include <asm/x86/syscall.h>
#include <asm/xol.h>
#include <asm/ioport.h>
#include <uapi/asm/prctl.h>
#include <uapi/asm/unistd_64.h>

#ifdef CONFIG_DEBUG_PROC_SWITCH
#define dbg_create(p) pr_info("create process %d\n", p)
#define dbg_switch(from, to) pr_info("switch from %d to %d\n", from, to)
#define dbg_exit(p) pr_info("exit process %d\n", p)
#else
#define dbg_create(p)
#define dbg_switch(from, to)
#define dbg_exit(p)
#endif

DEFINE_PER_CPU_ALIGNED(struct tls_pcpu, tls_pcpu) = {
	.current_task = &init_task,
	.stack_top = (unsigned long)init_stack + THREAD_SIZE,
	.uintr_stack_top = (unsigned long)init_stack + (THREAD_SIZE / 2),
	.preempt_count = INIT_PREEMPT_COUNT,
	.current_proc_key = 0,
	.host_tid = -1,
	.in_kernel = true,
};
EXPORT_PER_CPU_SYMBOL(tls_pcpu);

static inline int task_proc_key(const struct task_struct *task)
{
	struct mm_struct *mm = task->mm;
	return mm ? mm->context.proc_key : 0;
}

/* PR_SET_TSC applies to the Linux thread; Flux task state is tracked separately. */
DEFINE_PER_CPU(bool, flux_host_tsc_disabled);

static long flux_host_set_tsc_mode(unsigned int val)
{
	long ret = host_syscall(__NR_prctl, PR_SET_TSC, val, 0, 0, 0, 0);

	if (!ret)
		this_cpu_write(flux_host_tsc_disabled,
			       val == PR_TSC_SIGSEGV);
	return ret;
}

bool flux_tsc_enter_kernel_mode(void)
{
	bool restore = this_cpu_read(flux_host_tsc_disabled);

	if (restore)
		flux_host_set_tsc_mode(PR_TSC_ENABLE);
	return restore;
}

void flux_tsc_restore_user_mode(bool restore)
{
	if (restore)
		flux_host_set_tsc_mode(PR_TSC_SIGSEGV);
}

void disable_TSC(void)
{
	set_thread_flag(TIF_NOTSC);
}

void enable_TSC(void)
{
	clear_thread_flag(TIF_NOTSC);
	flux_host_set_tsc_mode(PR_TSC_ENABLE);
}

int get_tsc_mode(unsigned long adr)
{
	unsigned int val;

	if (test_thread_flag(TIF_NOTSC))
		val = PR_TSC_SIGSEGV;
	else
		val = PR_TSC_ENABLE;

	return put_user(val, (unsigned int __user *)adr);
}

int set_tsc_mode(unsigned int val)
{
	if (val == PR_TSC_SIGSEGV) {
		disable_TSC();
	} else if (val == PR_TSC_ENABLE) {
		enable_TSC();
	} else {
		return -EINVAL;
	}

	return 0;
}

int copy_fpstate(struct task_struct *p)
{
	struct xregs_state *child_xstate = task_xstate(p);
	struct xregs_state *current_xstate = task_xstate(current);

	/* Eager entry saves must survive fork's host calls and scheduling. */
	if (!test_thread_flag(TIF_NEED_FPU_LOAD)) {
		save_xstate_full(current_xstate);
		set_thread_flag(TIF_NEED_FPU_LOAD);
	}
	memcpy(child_xstate, current_xstate, flux_xstate_copy_size);
	set_tsk_thread_flag(p, TIF_NEED_FPU_LOAD);
	return 0;
}

int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	memcpy(dst, src, arch_task_struct_size);
#ifdef CONFIG_FLUX_MPK
	dst->thread.xol = NULL;
	if (unlikely(src->thread.io_bitmap))
		flux_io_bitmap_share(dst);
#endif
	return 0;
}

void arch_release_task_struct(struct task_struct *tsk)
{
	flux_xol_release(tsk);
#ifdef CONFIG_FLUX_MPK
	if (unlikely(tsk->thread.io_bitmap))
		flux_io_bitmap_exit(tsk);
#endif
}

int flux_init_host_mm(struct mm_struct *mm, bool for_fork)
{
	mm_context_t *ctx = &mm->context;
	int proc_key;

	if (WARN_ON_ONCE(ctx->proc_key))
		return -EINVAL;

	proc_key = flux_host_dev_copy_mm(for_fork);
	if (proc_key < 0) {
		pr_warn_ratelimited("create proc failed: err=%d\n", proc_key);
		return proc_key;
	}

	ctx->proc_key = proc_key;
	dbg_create(proc_key);
	return 0;
}

void flux_release_host_mm(struct mm_struct *mm)
{
	mm_context_t *ctx = &mm->context;

	if (!ctx->proc_key)
		return;

	dbg_exit(ctx->proc_key);
	flux_host_dev_release_mm(ctx->proc_key);
	ctx->proc_key = 0;
}

int init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	mm_context_t *ctx = &mm->context;
	int ret;

	mm->task_size = TASK_SIZE;
	WARN_ON(mm == &init_mm);
	ctx->proc_key = 0;
	mutex_init(&ctx->host_rewrite_mutex);
	mutex_init(&ctx->host_rewrite_state_mutex);
	seqlock_init(&ctx->host_rewrite_range_lock);
	ctx->host_rewrite_start = 0;
	ctx->host_rewrite_end = 0;
	if (!test_tsk_thread_flag(tsk, TIF_USER))
		return 0;

	/* Fork copies its host mm later, inside dup_mmap's Flux write locks. */
	if (tsk == current) {
		ret = flux_init_host_mm(mm, false);
		if (ret)
			return ret;
	}
	return 0;
}

void destroy_context(struct mm_struct *mm)
{
	mm_context_t *ctx = &mm->context;

	flux_release_host_mm(mm);
	mutex_destroy(&ctx->host_rewrite_state_mutex);
	mutex_destroy(&ctx->host_rewrite_mutex);
}

#define FLUX_UINTR_RETURN_PREFAULT_SIZE \
	(128 + sizeof(unsigned long) + 160 + \
	 4 * sizeof(unsigned long) + 15)

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	unsigned long clone_flags = args->flags;
	unsigned long sp = args->stack;
	unsigned long tls = args->tls;
	struct pt_regs *regs;
	struct fork_frame *fork_frame;
	struct inactive_task_frame *frame;

#ifdef CONFIG_FLUX_MPK
	p->thread.mpk_uaccess_depth = 0;
#endif

	p->thread.fault_signal = 0;

	regs = p->thread.regs = task_sys_regs(p);
	fork_frame = container_of(regs, struct fork_frame, regs);
	frame = &fork_frame->frame;
	frame->bp = encode_frame_pointer(regs);
	frame->ret_addr = (unsigned long)ret_from_fork_asm;
	p->thread.sp = (unsigned long)fork_frame;

	copy_fpstate(p);

	if (likely(args->fn)) {
		if (unlikely(args->idle))
			return 0;
		memset(regs, 0, sizeof(struct pt_regs));
		kthread_frame_init(frame, args->fn, args->fn_arg);
		return 0;
	}

	frame->bx = 0;
	/*
	 * Fork-family syscalls always build their complete user frame in the fixed
	 * task-top slot. current->thread.regs may transiently name a lower-half
	 * UINTR frame and must not become the child's persistent return context.
	 */
	memcpy(regs, task_sys_regs(current), sizeof(struct pt_regs));
	regs->ax = 0;
	if (sp)
		regs->sp = sp;

	/*
	 * A CLONE_VM child can receive its first UINTR before it has touched the
	 * stack supplied to clone().  Materialize that stack while the parent is
	 * still in ordinary Flux kernel context, rather than later from a host
	 * POSIX fault frame whose captured UIF must remain clear until
	 * rt_sigreturn.
	 *
	 * The deepest return boundary starts 128 bytes below the child RSP for
	 * the SysV red zone, consumes one flags word, then needs the 160-byte
	 * UISTACKADJUST and the four-qword hardware frame.  The safe helper walks
	 * the Flux MM directly, so it may sleep here without suspending a host
	 * signal transaction; the set_ptes hook installs the matching host alias.
	 *
	 * Keep this best-effort.  Native clone does not eagerly reject an invalid
	 * child stack, and the eventual access must retain that behavior.
	 */
	sp = regs->sp;
	if ((clone_flags & CLONE_VM) &&
	    sp >= FLUX_UINTR_RETURN_PREFAULT_SIZE)
		(void)fault_in_safe_writeable(
			(char __user *)(sp - FLUX_UINTR_RETURN_PREFAULT_SIZE),
			FLUX_UINTR_RETURN_PREFAULT_SIZE);


	if (clone_flags & CLONE_SETTLS)
		p->thread.fsbase = tls;

	return 0;
}

void flush_thread(void)
{
	flux_xol_release(current);
	current->thread.fsbase = 0;
	wrfsbase(flux_host_fsbase());
	memcpy(task_xstate(current), &init_task_xstate, PAGE_SIZE);
	set_thread_flag(TIF_NEED_FPU_LOAD);
}

void start_thread(struct pt_regs *regs, unsigned long new_ip,
		  unsigned long new_sp)
{
	set_thread_flag(TIF_USER);
	current->flags &= ~PF_KTHREAD;
	memset(regs, 0, sizeof(*regs));
	regs->ip = new_ip;
	regs->sp = new_sp;
	regs->flags = X86_EFLAGS_FIXED;
	regs->umode = 1;
	regs->orig_ax = -1;
}

/* exec_mmap publishes the new LibOS mm with interrupts disabled. */
void activate_mm(struct mm_struct *prev, struct mm_struct *next)
{
	int from = current_proc_key();
	int to = next->context.proc_key;

	if (from == to)
		return;
	BUG_ON(flux_host_dev_switch_mm(to, from));
	raw_cpu_write(tls_pcpu.current_proc_key, to);
}

void __switch_mm(struct task_struct *prev, struct task_struct *next)
{
	int prev_proc_key = current_proc_key();
	int next_proc_key = task_proc_key(next);
	int err;

	if (likely(prev_proc_key == next_proc_key))
		return;

	/*
	 * Switch address space: -> user (kernel threads are shared in Flux Kernel).
	 * We don't use task_proc_key(prev_p) because we may switch from a kernel
	 * thread whose proc_key is the same as the next thread but in a different
	 * host mm. We only switch back to proc 0 when the previous task is exiting;
	 * release_mm() may already have been issued, but the host keeps the old
	 * proc visible until this final switch drops its last active user.
	 */
	if (test_tsk_thread_flag(next, TIF_USER)) {
		if (next_proc_key != prev_proc_key)
			goto do_switch;
	} else if (prev_proc_key != 0 &&
		   (READ_ONCE(prev->exit_state) & (EXIT_DEAD | EXIT_ZOMBIE))) {
		/* Kernel threads should run on default proc 0. */
		next_proc_key = 0;
		goto do_switch;
	}

	return;
do_switch:
	dbg_switch(prev_proc_key, next_proc_key);
	err = flux_host_dev_switch_mm(next_proc_key, prev_proc_key);
	if (unlikely(err)) {
		pr_warn_ratelimited("switch_mm failed: from=%d to=%d err=%d\n",
				    prev_proc_key, next_proc_key, err);
		BUG();
	} else {
		/* Update current running process only on success. */
		raw_cpu_write(tls_pcpu.current_proc_key, next_proc_key);
	}
}

__no_kmsan_checks struct task_struct *__switch_to(struct task_struct *prev_p,
						  struct task_struct *next_p)
{
	WARN_ON(current != prev_p);

	raw_cpu_write(tls_pcpu.current_task, next_p);
	raw_cpu_write(tls_pcpu.stack_top, task_stack_top(next_p));
	raw_cpu_write(tls_pcpu.uintr_stack_top, uintr_stack_top(next_p));

	__switch_mm(prev_p, next_p);

	/*
	 * Resume kernel/Env C code with usable TLS. TIF_USER also marks the Env
	 * bootstrap; only the application exit hook installs a zero user FSBASE.
	 */
	if (next_p->thread.fsbase)
		wrfsbase(next_p->thread.fsbase);
	else
		wrfsbase(flux_host_fsbase());

	return prev_p;
}

__visible void ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
			     int (*fn)(void *), void *fn_arg)
{
	int ret;

	schedule_tail(prev);

	/* Is this a kernel thread? */
	if (unlikely(fn)) {
		/* kthreads managed by kthreadd will do_exit() inside kthread() */
		ret = fn(fn_arg);

		/*
		 * A kernel thread is allowed to return here after successfully
		 * calling kernel_execve().  Exit to userspace to complete the
		 * execve() syscall.
		 */
		regs->ax = 0;

		if (ret || !regs->umode)
			do_exit(ret);
	}
	syscall_exit_to_user_mode(regs);
	syscall_ret_to_user(regs);
}

void arch_cpu_idle_enter(void)
{
	/*
	 * A synchronous host signal may schedule while kmod has quarantined the
	 * receiver state until rt_sigreturn. If the signal-side task switches to
	 * idle, schedule() clears any NEED edge posted before that switch. Recreate
	 * the edge at the post-schedule, pre-idle boundary so the generic idle
	 * protocol consumes an already queued CALLFUNC wakeup. This neither drains
	 * the queue here nor reads or changes hardware UIF.
	 */
	if (unlikely(this_cpu_read(tls_pcpu.host_signal_depth)))
		set_tsk_need_resched(current);
}

SYSCALL_DEFINE2(arch_prctl, int, option, unsigned long, arg2)
{
	int ret = 0;

	switch (option) {
	case ARCH_SET_FS:
		preempt_disable();

		wrfsbase(arg2);

		current->thread.fsbase = arg2;

		preempt_enable();
		break;
	case ARCH_GET_FS:
		ret = put_user(current->thread.fsbase,
			       (unsigned long __user *)arg2);

		break;
	case ARCH_SET_CPUID:
		ret = -ENODEV;
		break;
	case ARCH_GET_CPUID:
		ret = 1;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
