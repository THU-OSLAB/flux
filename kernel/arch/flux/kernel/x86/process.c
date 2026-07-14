#define pr_fmt(fmt) "process: " fmt

#include <linux/entry-common.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/module.h>
#include <linux/mm_types.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <asm/irq.h>
#include <asm/syscalls.h>
#include <asm/host_dev.h>
#include <asm/current.h>
#include <asm/mmu.h>
#include <asm/x86/ptrace.h>
#include <asm/x86/switch_to.h>
#include <asm/x86/fpu.h>
#include <asm/x86/uintr.h>
#include <asm/x86/cpuid.h>
#include <asm/x86/processor.h>
#include <asm/x86/syscall.h>
#include <uapi/asm/prctl.h>

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

int copy_fpstate(struct task_struct *p)
{
#ifdef CONFIG_FLUX_UINTR
	save_xstate(task_xstate(p));
#endif
	return 0;
}

int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	memcpy(dst, src, arch_task_struct_size);
	return 0;
}

int init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	mm_context_t *ctx = &mm->context;
	int proc_key;

	WARN_ON(mm == &init_mm);
	if (!test_tsk_thread_flag(tsk, TIF_USER))
		return 0;

	proc_key = flux_host_dev_copy_mm();
	if (proc_key < 0) {
		pr_warn_ratelimited("create proc failed: err=%d\n", proc_key);
		return proc_key;
	}
	ctx->proc_key = proc_key;
	dbg_create(proc_key);
	return 0;
}

void destroy_context(struct mm_struct *mm)
{
	mm_context_t *ctx = &mm->context;

	if (ctx->proc_key) {
		dbg_exit(ctx->proc_key);
		flux_host_dev_release_mm(ctx->proc_key);
		ctx->proc_key = 0;
	}
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	unsigned long clone_flags = args->flags;
	unsigned long sp = args->stack;
	unsigned long tls = args->tls;
	struct pt_regs *regs;
	struct fork_frame *fork_frame;
	struct inactive_task_frame *frame;

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
	memcpy(regs, current->thread.regs, sizeof(struct pt_regs));
	regs->ax = 0;
	if (sp)
		regs->sp = sp;

	WARN_ON(!sp && (clone_flags & CLONE_VM));

	if (clone_flags & CLONE_SETTLS)
		p->thread.fsbase = tls;

	return 0;
}

void start_thread(struct pt_regs *regs, unsigned long new_ip,
		  unsigned long new_sp)
{
	regs->ip = new_ip;
	regs->sp = new_sp;
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

#ifdef CONFIG_FLUX_UINTR
	/*
	 * Save FPU/SIMD state when switching away from a user thread that
	 * was interrupted by UINTR while in user mode.  The flag
	 * TIF_UINTR_FROM_USER is set by uintr_handler() when it
	 * interrupts user-mode code; it tells us the hardware YMM/XMM
	 * registers still hold the user's live values.
	 *
	 * Syscalls enter via a plain C function call (flux_syscall_fast),
	 * so the caller-saved FPU registers are already on the user stack
	 * per the x86-64 ABI — no kernel-side save needed.
	 *
	 * We must NOT use in_hardirq() here: by the time __switch_to()
	 * runs the hardirq context has already been exited by
	 * irq_exit_rcu(), so in_hardirq() is always false.
	 */
	if (!test_tsk_thread_flag(prev_p, TIF_NEED_FPU_LOAD) &&
	    test_tsk_thread_flag(prev_p, TIF_UINTR_FROM_USER)) {
		save_xstate(task_xstate(prev_p));
		set_tsk_thread_flag(prev_p, TIF_NEED_FPU_LOAD);
	}
#endif

	__switch_mm(prev_p, next_p);

	/* Switch thread local storage for user threads. */
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

		/* avoid stack overflow */
		do_exit(ret);
	} else {
		syscall_exit_to_user_mode(regs);
		syscall_ret_to_user(regs);
	}
}

void arch_cpu_idle(void)
{
	cpu_relax();
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
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
