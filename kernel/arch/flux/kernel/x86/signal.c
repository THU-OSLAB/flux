// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/unistd.h>
#include <linux/stddef.h>
#include <linux/personality.h>
#include <linux/uaccess.h>
#include <linux/user-return-notifier.h>
#include <linux/uprobes.h>
#include <linux/context_tracking.h>
#include <linux/entry-common.h>
#include <linux/syscalls.h>
#include <asm/processor.h>
#include <asm/ucontext.h>
#include <uapi/asm/signal.h>
#include <asm/signal.h>
#include <asm/x86/syscall.h>
#include <asm/x86/sighandling.h>
#include <asm/x86/sigframe.h>

static unsigned long alloc_fpstate(unsigned long sp, unsigned long *buf_fx,
				   unsigned long *size)
{
	unsigned long frame_size = PAGE_SIZE;

	*buf_fx = sp = round_down(sp - frame_size, 64);

	*size = frame_size;

	return sp;
}

/* x86 ABI requires 16-byte alignment */
#define FRAME_ALIGNMENT 16UL

#define MAX_FRAME_PADDING (FRAME_ALIGNMENT - 1)

int show_unhandled_signals = 1;

/*
 * Determine which stack to use..
 */
void __user *get_sigframe(struct ksignal *ksig, struct pt_regs *regs,
			  size_t frame_size, void __user **fpstate)
{
	struct k_sigaction *ka = &ksig->ka;
	/* Default to using normal stack */
	bool nested_altstack = on_sig_stack(regs->sp);
	bool entering_altstack = false;
	unsigned long math_size = 0;
	unsigned long sp = regs->sp;
	unsigned long buf_fx = 0;
	struct xregs_state __user *xbuf;

	/* redzone */
	sp -= 128;

	/* reserve space for pt_regs of uintr */
	if (test_thread_flag(TIF_UINTR_FROM_USER)) {
		sp -= sizeof(struct pt_regs);
		sp = round_down(sp, FRAME_ALIGNMENT);
		WARN_ON(sp > (unsigned long)regs);
	}

	/* This is the X/Open sanctioned signal stack switching.  */
	if (ka->sa.sa_flags & SA_ONSTACK) {
		/*
		 * This checks nested_altstack via sas_ss_flags(). Sensible
		 * programs use SS_AUTODISARM, which disables that check, and
		 * programs that don't use SS_AUTODISARM get compatible.
		 */
		if (sas_ss_flags(sp) == 0) {
			sp = current->sas_ss_sp + current->sas_ss_size;
			entering_altstack = true;
		}
	}

	sp = alloc_fpstate(sp, &buf_fx, &math_size);
	*fpstate = (void __user *)sp;
	xbuf = (void *)buf_fx;

	/* Align the stack pointer */
	sp -= frame_size;
	sp = round_down(sp, FRAME_ALIGNMENT) - 8;

	/*
	 * If we are on the alternate signal stack and would overflow it, don't.
	 * Return an always-bogus address instead so we will die with SIGSEGV.
	 */
	if (unlikely((nested_altstack || entering_altstack) &&
		     !__on_sig_stack(sp))) {
		if (show_unhandled_signals && printk_ratelimit())
			pr_info("%s[%d] overflowed sigaltstack\n",
				current->comm, task_pid_nr(current));

		return (void __user *)-1L;
	}

	if (!access_ok(*fpstate, math_size))
		return (void __user *)-1L;

	if (test_thread_flag(TIF_NEED_FPU_LOAD)) {
		restore_xstate(task_xstate(current));
		clear_thread_flag(TIF_NEED_FPU_LOAD);
	}

	if (__clear_user(&xbuf->header, sizeof(xbuf->header)))
		return (void __user *)-1L;
	save_xstate((void *)xbuf);

	return (void __user *)sp;
}

static unsigned long frame_uc_flags(struct pt_regs *regs)
{
	unsigned long flags;

	/* x86-64 always has XSAVE */
	flags = UC_FP_XSTATE | UC_SIGCONTEXT_SS;

	/* 64-bit mode needs strict SS restore */
	flags |= UC_STRICT_RESTORE_SS;

	return flags;
}

static __always_inline int
__unsafe_setup_sigcontext(struct sigcontext __user *sc, void __user *fpstate,
			  struct pt_regs *regs, unsigned long mask)
{
	unsafe_put_user(regs->di, &sc->di, efault);
	unsafe_put_user(regs->si, &sc->si, efault);
	unsafe_put_user(regs->bp, &sc->bp, efault);
	unsafe_put_user(regs->sp, &sc->sp, efault);
	unsafe_put_user(regs->bx, &sc->bx, efault);
	unsafe_put_user(regs->dx, &sc->dx, efault);
	unsafe_put_user(regs->cx, &sc->cx, efault);
	unsafe_put_user(regs->ax, &sc->ax, efault);
	unsafe_put_user(regs->r8, &sc->r8, efault);
	unsafe_put_user(regs->r9, &sc->r9, efault);
	unsafe_put_user(regs->r10, &sc->r10, efault);
	unsafe_put_user(regs->r11, &sc->r11, efault);
	unsafe_put_user(regs->r12, &sc->r12, efault);
	unsafe_put_user(regs->r13, &sc->r13, efault);
	unsafe_put_user(regs->r14, &sc->r14, efault);
	unsafe_put_user(regs->r15, &sc->r15, efault);

	unsafe_put_user(0, &sc->trapno, efault);
	unsafe_put_user(0, &sc->err, efault);
	unsafe_put_user(regs->ip, &sc->ip, efault);
	unsafe_put_user(regs->flags, &sc->flags, efault);
	unsafe_put_user(0x33, &sc->cs, efault);
	unsafe_put_user(0, &sc->gs, efault);
	unsafe_put_user(0, &sc->fs, efault);
	unsafe_put_user(0x2b, &sc->ss, efault);

	unsafe_put_user(fpstate, (void **)&sc->fpstate, efault);

	/* non-iBCS2 extensions.. */
	unsafe_put_user(mask, &sc->oldmask, efault);
	unsafe_put_user(0, &sc->cr2, efault);
	return 0;
efault:
	return -EFAULT;
}

#define unsafe_put_sigcontext(sc, fp, regs, set, label)                   \
	do {                                                              \
		if (__unsafe_setup_sigcontext(sc, fp, regs, set->sig[0])) \
			goto label;                                       \
	} while (0);

#define unsafe_put_sigmask(set, frame, label) \
	unsafe_put_user(*(__u64 *)(set),      \
			(__u64 __user *)&(frame)->uc.uc_sigmask, label)

int setup_rt_frame(struct ksignal *ksig, struct pt_regs *regs)
{
	sigset_t *set = sigmask_to_save();
	struct rt_sigframe __user *frame;
	void __user *fp = NULL;
	unsigned long uc_flags;

	/* x86-64 should always use SA_RESTORER. */
	if (!(ksig->ka.sa.sa_flags & SA_RESTORER))
		return -EFAULT;

	frame = get_sigframe(ksig, regs, sizeof(struct rt_sigframe), &fp);
	uc_flags = frame_uc_flags(regs);

	if (!user_access_begin(frame, sizeof(*frame)))
		return -EFAULT;

	/* Create the ucontext.  */
	unsafe_put_user(uc_flags, &frame->uc.uc_flags, fault);
	unsafe_put_user(0, &frame->uc.uc_link, fault);
	unsafe_save_altstack(&frame->uc.uc_stack, regs->sp, fault);

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	unsafe_put_user(ksig->ka.sa.sa_restorer,
			(__sigrestore_t *)&frame->pretcode, fault);
	unsafe_put_sigcontext(&frame->uc.uc_mcontext, (void __user *)fp, regs,
			      set, fault);
	unsafe_put_sigmask(set, frame, fault);
	user_access_end();

	if (ksig->ka.sa.sa_flags & SA_SIGINFO)
		if (copy_siginfo_to_user(&frame->info, &ksig->info))
			return -EFAULT;

	/* Set up registers for signal handler */
	regs->di = ksig->sig;
	/* In case the signal handler was declared without prototypes */
	regs->ax = 0;

	/* This also works for non SA_SIGINFO handlers because they expect the
	   next argument after the signal number on the stack. */
	regs->si = (unsigned long)&frame->info;
	regs->dx = (unsigned long)&frame->uc;
	regs->ip = (unsigned long)ksig->ka.sa.sa_handler;

	regs->sp = (unsigned long)frame;

	return 0;

fault:
	user_access_end();
	return -EFAULT;
}

// #define RESTART_SYSCALL_INSTRUCTION_SIZE 2
/* ff 14 25 00 00 10 00 	call   *0x100000 */
/* ff 14 25 08 00 10 00 	call   *0x100008 */
#define RESTART_SYSCALL_INSTRUCTION_SIZE 7

static void handle_signal(struct ksignal *ksig, struct pt_regs *regs)
{
	bool failed;

	/* Are we from a system call? */
	if (syscall_get_nr(current, regs) != -1) {
		/* If so, check system call restarting.. */
		switch (syscall_get_error(current, regs)) {
		case -ERESTART_RESTARTBLOCK:
		case -ERESTARTNOHAND:
			regs->ax = -EINTR;
			break;

		case -ERESTARTSYS:
			if (!(ksig->ka.sa.sa_flags & SA_RESTART)) {
				regs->ax = -EINTR;
				break;
			}
			fallthrough;
		case -ERESTARTNOINTR:
			regs->ax = regs->orig_ax;
			regs->ip -= RESTART_SYSCALL_INSTRUCTION_SIZE;
			break;
		}
	}

	failed = setup_rt_frame(ksig, regs) < 0;
	if (!failed) {
		/*
		 * Clear the direction flag as per the ABI for function entry.
		 *
		 * Clear RF when entering the signal handler, because
		 * it might disable possible debug exception from the
		 * signal handler.
		 *
		 * Clear TF for the case when it wasn't set by debugger to
		 * avoid the recursive send_sigtrap() in SIGTRAP handler.
		 */
		regs->flags &= ~(X86_EFLAGS_DF | X86_EFLAGS_RF | X86_EFLAGS_TF);
		/*
		 * Ensure the signal handler starts with the new fpu state.
		 */
		memcpy(task_xstate(current), &init_task_xstate, PAGE_SIZE);
		set_thread_flag(TIF_NEED_FPU_LOAD);
	}
	signal_setup_done(failed, ksig, false);
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 */
void arch_do_signal_or_restart(struct pt_regs *regs)
{
	struct ksignal ksig;

	if (get_signal(&ksig)) {
		/* Whee! Actually deliver the signal.  */
		handle_signal(&ksig, regs);
		return;
	}

	/* Did we come from a system call? */
	if (syscall_get_nr(current, regs) != -1) {
		/* Restart the system call - no handlers present */
		switch (syscall_get_error(current, regs)) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
			regs->ax = regs->orig_ax;
			regs->ip -= RESTART_SYSCALL_INSTRUCTION_SIZE;
			break;

		case -ERESTART_RESTARTBLOCK:
			regs->ax = __NR_restart_syscall;
			regs->ip -= RESTART_SYSCALL_INSTRUCTION_SIZE;
			break;
		}
	}

	/*
	 * If there's no signal to deliver, we just put the saved sigmask
	 * back.
	 */
	restore_saved_sigmask();
}

static inline void syscall_fast_ret_to_user(struct pt_regs *regs)
{
	local_irq_disable_exit_to_user();
	if (test_thread_flag(TIF_NEED_FPU_LOAD)) {
		restore_xstate(task_xstate(current));
		clear_thread_flag(TIF_NEED_FPU_LOAD);
	}
	arch_exit_to_user_mode();
	syscall_ret_to_user(regs);
}

void syscall_fast_do_signal_or_restart(struct pt_regs *regs)
{
	struct ksignal ksig;
	bool restart = false;

	if (get_signal(&ksig)) {
		/* Whee! Actually deliver the signal.  */
		handle_signal(&ksig, regs);
		syscall_fast_ret_to_user(regs);
	}

	/* Did we come from a system call? */
	if (syscall_get_nr(current, regs) != -1) {
		/* Restart the system call - no handlers present */
		switch (syscall_get_error(current, regs)) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
			regs->ax = regs->orig_ax;
			regs->ip -= RESTART_SYSCALL_INSTRUCTION_SIZE;
			restart = true;
			break;

		case -ERESTART_RESTARTBLOCK:
			regs->ax = __NR_restart_syscall;
			regs->ip -= RESTART_SYSCALL_INSTRUCTION_SIZE;
			restart = true;
			break;
		}
	}

	/*
	 * If there's no signal to deliver, we just put the saved sigmask
	 * back.
	 */
	restore_saved_sigmask();

	if (restart)
		syscall_fast_ret_to_user(regs);
}

void signal_fault(struct pt_regs *regs, void __user *frame, char *where)
{
	struct task_struct *me = current;

	if (show_unhandled_signals && printk_ratelimit()) {
		printk("%s"
		       "%s[%d] bad frame in %s frame:%p ip:%llx sp:%llx orax:%llx",
		       task_pid_nr(current) > 1 ? KERN_INFO : KERN_EMERG,
		       me->comm, me->pid, where, frame, regs->ip, regs->sp,
		       regs->orig_ax);
		print_vma_addr(KERN_CONT " in ", regs->ip);
		pr_cont("\n");
	}

	force_sig(SIGSEGV);
}

/*
 * Restore FPU state from a sigframe:
 */
bool restore_fpstate(void __user *buf)
{
	bool success = false;
	unsigned int size = PAGE_SIZE;

	if (!access_ok(buf, size))
		goto out;

	if (!test_thread_flag(TIF_NEED_FPU_LOAD))
		set_thread_flag(TIF_NEED_FPU_LOAD);

	if (copy_from_user(task_xstate(current), buf, size))
		goto out;

	return true;
out:
	if (unlikely(!success)) {
		memcpy(task_xstate(current), &init_task_xstate, PAGE_SIZE);
		set_thread_flag(TIF_NEED_FPU_LOAD);
	}
	return success;
}

static bool restore_sigcontext(struct pt_regs *regs,
			       struct sigcontext __user *usc,
			       unsigned long uc_flags)
{
	struct sigcontext sc;

	/* Always make any pending restarted system calls return -EINTR */
	current->restart_block.fn = do_no_restart_syscall;

	if (copy_from_user(&sc, usc, offsetof(struct sigcontext, reserved1)))
		return false;

	regs->bx = sc.bx;
	regs->cx = sc.cx;
	regs->dx = sc.dx;
	regs->si = sc.si;
	regs->di = sc.di;
	regs->bp = sc.bp;
	regs->ax = sc.ax;
	regs->sp = sc.sp;
	regs->ip = sc.ip;
	regs->r8 = sc.r8;
	regs->r9 = sc.r9;
	regs->r10 = sc.r10;
	regs->r11 = sc.r11;
	regs->r12 = sc.r12;
	regs->r13 = sc.r13;
	regs->r14 = sc.r14;
	regs->r15 = sc.r15;

	regs->flags = (regs->flags & ~FIX_EFLAGS) | (sc.flags & FIX_EFLAGS);
	/* disable syscall checks */
	regs->orig_ax = -1;

	return restore_fpstate((void __user *)sc.fpstate);
}

/*
 * Do a signal return; undo the signal stack.
 */
SYSCALL_DEFINE0(rt_sigreturn)
{
	struct pt_regs *regs = current_pt_regs();
	struct rt_sigframe __user *frame;
	sigset_t set;
	unsigned long uc_flags;

	frame = (struct rt_sigframe __user *)(regs->sp - sizeof(long));
	if (!access_ok(frame, sizeof(*frame)))
		goto badframe;
	if (__get_user(*(__u64 *)&set, (__u64 __user *)&frame->uc.uc_sigmask))
		goto badframe;
	if (__get_user(uc_flags, &frame->uc.uc_flags))
		goto badframe;

	set_current_blocked(&set);

	if (!restore_sigcontext(regs, &frame->uc.uc_mcontext, uc_flags))
		goto badframe;

	if (restore_altstack(&frame->uc.uc_stack))
		goto badframe;

	return regs->ax;

badframe:
	signal_fault(regs, frame, "rt_sigreturn");
	return 0;
}
