// SPDX-License-Identifier: GPL-2.0+
#define pr_fmt(fmt) "uintr: " fmt

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/entry-common.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/pid.h>
#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/irqflags.h>
#include <asm/processor.h>
#include <asm/x86/fpu.h>
#include <asm/x86/syscall.h>
#include <asm/xol.h>
#include <asm/signal.h>
#include <uapi/asm/flux_ops.h>
#include <uapi/asm/flux_oci.h>
#include <uapi/asm/mpk.h>
#include <asm/x86/uintr.h>

extern int uintr_timer_irq;

#define UINTR_IS(name, v) ((v) == (FLUX_UINTR_VECTOR_##name))

struct flux_uipi_pcpu flux_uipi[NR_CPUS] __read_mostly;

extern char __flux_syscall_fast_end[];
extern char __flux_syscall_rewrite_fast_end[];
extern void flux_syscall_rewrite_fast(void);

static __always_inline bool flux_uintr_on_user_return_path(unsigned long ip)
{
	return flux_syscall_on_user_return_path(ip) ||
	       (ip >= (unsigned long)flux_syscall_fast &&
		ip < (unsigned long)__flux_syscall_fast_end) ||
	       (ip >= (unsigned long)flux_syscall_rewrite_fast &&
		ip < (unsigned long)__flux_syscall_rewrite_fast_end);
}

static irqreturn_t ipi_handler(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static int flux_ipi_set_affinity(struct irq_data *data,
				 const struct cpumask *dest, bool force)
{
	/*
	 * Flux IPIs are delivered to their explicit target CPU independently of
	 * the generic IRQ affinity mask.  Accept userspace policy updates so the
	 * generic IRQ core can publish the requested mask through procfs.
	 */
	return IRQ_SET_MASK_OK;
}

static struct irq_chip flux_ipi_irq_chip;

int flux_ipi_init(void)
{
	int ret;

	ret = request_irq(FLUX_IRQ_IPI, ipi_handler, IRQF_NO_THREAD, "flux_ipi",
			  NULL);
	if (ret < 0) {
		pr_err("failed to register IPI irq %d\n", ret);
		return ret;
	}

	return 0;
}

void init_IRQ(void)
{
	flux_ipi_irq_chip = dummy_irq_chip;
	flux_ipi_irq_chip.name = "flux-ipi";
	flux_ipi_irq_chip.irq_set_affinity = flux_ipi_set_affinity;
	irq_set_chip_and_handler(FLUX_IRQ_IPI, &flux_ipi_irq_chip,
				 handle_simple_irq);

	pr_info("irqs initialized\n");
}

void cpu_yield_to_irqs(void)
{
	cpu_relax();
}

/*
 * /proc/interrupts printing for arch specific interrupts
 */
int arch_show_interrupts(struct seq_file *p, int prec)
{
	return 0;
}

void flux_uintr_send_ipi(int from, int to, int vector)
{
	senduipi(
		flux_uipi[from].vecs[FLUX_UINTR_VEC_IPI_BASE + vector].uitt[to]);
}

void flux_uintr_send_uvec(int from, int to, int vector)
{
	senduipi(flux_uipi[from].vecs[vector].uitt[to]);
}

#ifdef CONFIG_SMP
void arch_smp_send_reschedule(int cpu)
{
	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, FLUX_IPI_RESCHED);
}

void arch_send_call_function_single_ipi(int cpu)
{
	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, FLUX_IPI_CALLFUNC);
}

void flux_tick_broadcast(int cpu)
{
	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, FLUX_IPI_TICKBC);
}

void flux_shutdown(int cpu)
{
	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, FLUX_IPI_SHUTDOWN);
}

static void handle_ipi(struct pt_regs *regs)
{
	unsigned long uirrv = regs->uirrv;
	unsigned long vec = uirrv - FLUX_UINTR_VECTOR_IPI;

	switch (vec) {
	case FLUX_IPI_RESCHED:
		scheduler_ipi();
		break;
	case FLUX_IPI_CALLFUNC:
		generic_smp_call_function_interrupt();
		break;
	case FLUX_IPI_TICKBC:
		tick_receive_broadcast();
		break;
	case FLUX_IPI_SHUTDOWN:
		flux_cpu_exit();
		break;
	default:
		pr_err("Unknown IPI vector: %lu\n", vec);
		break;
	}
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_SMP
void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	int cpu;

	for_each_cpu(cpu, mask) {
		arch_send_call_function_single_ipi(cpu);
	}
}
#endif /* CONFIG_SMP */

static void do_irq(struct pt_regs *regs, int irq)
{
	struct pt_regs *old_regs;

	irq_enter_rcu();
	old_regs = set_irq_regs(regs);
	generic_handle_irq(irq);
#ifdef CONFIG_SMP
	if (irq == FLUX_IRQ_IPI)
		handle_ipi(regs);
#endif
	set_irq_regs(old_regs);
	irq_exit_rcu();
}
static void do_signal_irq(struct pt_regs *regs)
{
	struct pt_regs *old_regs;
	struct flux_sig_entry *entry;
	struct llist_node *node = NULL;
	struct task_struct *target;
	kernel_siginfo_t info;
	int ret, cpu = raw_smp_processor_id();

	irq_enter_rcu();
	old_regs = set_irq_regs(regs);
	instrumentation_begin();

	/*
	 * Keep each detached batch cursor on this entry's stack. If a nested UINTR
	 * drains a later batch, it cannot consume or free this entry's nodes.
	 * Recheck the atomic head after every batch to drain arrivals that were not
	 * handled by a nested entry.
	 */
	for (;;) {
		if (!node) {
			node = flux_sig_take_batch(cpu);
			if (!node)
				break;
		}
		entry = llist_entry(node, struct flux_sig_entry, sig_node);
		node = node->next;

		target = NULL;

		if (IS_ENABLED(CONFIG_FLUX_RUNC) &&
		    (entry->ctrl_op == FLUX_SIGNAL_CTRL_EXEC ||
		     entry->ctrl_op == FLUX_SIGNAL_CTRL_RESOURCE)) {
			flux_exec_wake();
			goto out_entry;
		}

		/* A queued signal keeps its original PID even if that task exits. */
		target = get_pid_task(entry->sig_pid, PIDTYPE_PID);
		if (!target)
			goto out_entry;

		switch (entry->sig_nr) {
		case SIGSEGV:
		case SIGBUS:
		case SIGILL:
		case SIGFPE:
		case SIGTRAP:
			/*
			 * Preserve fault-style metadata for synchronous traps,
			 * so userspace receives a meaningful si_code/si_addr.
			 */
			ret = send_sig_fault(entry->sig_nr, entry->sig_code,
					     (void __user *)entry->sig_addr,
					     target);
			break;
		default:
			if (entry->sig_has_info) {
				clear_siginfo(&info);
				info.si_signo = entry->sig_nr;
				info.si_errno = entry->sig_info_errno;
				info.si_code = entry->sig_code;
				info.si_pid = entry->sig_info_pid;
				info.si_uid = entry->sig_info_uid;
				ret = send_sig_info(entry->sig_nr, &info, target);
			} else {
				ret = send_sig(entry->sig_nr, target, 1);
			}
			break;
		}
		if (ret)
			pr_warn("failed to inject signal %d (ret=%d)\n",
				entry->sig_nr, ret);

out_entry:
		if (entry->sig_pid)
			put_pid(entry->sig_pid);
		if (target)
			put_task_struct(target);
		kfree(entry);
	}

	instrumentation_end();
	set_irq_regs(old_regs);
	irq_exit_rcu();
}

void flux_uintr_defer_signal_delivery(int cpu)
{
	/*
	 * Signal entry has already cleared live UIF. Posting the real SIGNAL
	 * vector therefore records hardware-pending work without forcing entry.
	 * UIRR coalescing is safe because the per-CPU signal queue retains every
	 * payload; a genuine later STUI admits one UINTR which drains the queue.
	 */
	flux_uintr_send_uvec(cpu, cpu, FLUX_UINTR_VECTOR_SIGNAL);
}

static void flux_uintr_dispatch_vector(struct pt_regs *regs, u64 vector)
{
	u64 saved_uirrv = regs->uirrv;

	regs->uirrv = vector;

	if (UINTR_IS(TIMER, vector)) {
		do_irq(regs, uintr_timer_irq);
	} else if (UINTR_IS(SIGNAL, vector)) {
		do_signal_irq(regs);
	} else if (vector >= FLUX_UINTR_VECTOR_IPI &&
		   vector < FLUX_UINTR_VECTOR_IPI + FLUX_IPI_NR) {
		do_irq(regs, FLUX_IRQ_IPI);
	} else {
		pr_err("unknown UINTR vector: %llu\n",
		       (unsigned long long)vector);
	}
	regs->uirrv = saved_uirrv;
}

/*
 * UIRET has already completed the receiver entry and restored the interrupted
 * UIF before this continuation runs.  Finish generic exit work, including a
 * pending schedule, then use the ordinary JMP-based user return.  That return
 * path preserves live UIF and restores the context saved in this UINTR frame.
 */
struct flux_user_fault_exit {
	struct pt_regs *regs;
	struct pt_regs *old_regs;
	struct flux_deferred_fault fault;
};

asmlinkage __noreturn void uintr_user_exit(struct flux_user_fault_exit *context)
{
	struct flux_user_fault_exit saved = *context;
	struct pt_regs *regs = saved.regs;

	regs->uirrv = 0;
	flux_complete_user_fault(regs, &saved.fault);
	irqentry_exit_to_user_mode(regs);
	current->thread.regs = saved.old_regs;
	syscall_ret_to_user(regs);
	unreachable();
}

/*
 * User FPU and FSBASE state belongs to the task. Any sleepable fault, signal or
 * reschedule work runs through uintr_user_exit, after UIRET restores UIF.
 */
static __noreturn void uintr_handle_user(struct pt_regs *regs,
				       unsigned long interrupted_fsbase,
				       const struct flux_deferred_fault *fault)
{
	struct pt_regs *old_regs = current->thread.regs;
	struct xregs_state *xstate = task_xstate(current);

#ifdef CONFIG_FLUX_MPK
	if (unlikely(current->thread.xol))
		flux_xol_interrupt(regs);
#endif
	current->thread.fsbase = interrupted_fsbase;
	current->thread.regs = regs;
	memset(&xstate->header, 0, sizeof(xstate->header));
	save_xstate_full(xstate);
	set_thread_flag(TIF_NEED_FPU_LOAD);
	flux_tsc_enter_kernel_mode();

	irqentry_enter(regs);
	flux_uintr_dispatch_vector(regs, regs->uirrv & ~FLUX_UINTR_SYNTHETIC_FLAG);

	if (fault->signal || (read_thread_flags() & EXIT_TO_USER_MODE_WORK)) {
		struct flux_user_fault_exit context = {
			.regs = regs,
			.old_regs = old_regs,
			.fault = *fault,
		};

		__ret_from_uintr_user_exit(&context);
	}

	arch_exit_to_user_mode_prepare(regs, read_thread_flags());
	current->thread.regs = old_regs;
	exit_to_user_mode();
	__ret_from_uintr(regs
#ifdef CONFIG_FLUX_MPK
			 , FLUX_MPK_APP_PKRU
#endif
	);
}

struct flux_kernel_fault_exit {
	struct flux_deferred_fault fault;
	struct pt_regs *regs;
	struct pt_regs *old_regs;
	struct xregs_state *xstate;
	unsigned long fsbase;
	bool restore_in_kernel;
	bool restore_tsc;
};

/* UIRET has ended the synthetic entry before any fault work can sleep. */
asmlinkage __noreturn void uintr_kernel_fault_exit(
				struct flux_kernel_fault_exit *context)
{
	struct flux_kernel_fault_exit saved = *context;

	flux_complete_kernel_fault(saved.regs, &saved.fault);
	preempt_check_resched();
	current->thread.regs = saved.old_regs;
	if (saved.restore_in_kernel)
		this_cpu_write(tls_pcpu.in_kernel, false);
	wrfsbase(saved.fsbase);
	flux_tsc_restore_user_mode(saved.restore_tsc);
	BUG_ON(restore_xstate(saved.xstate));
	__ret_from_kernel_fault(saved.regs);
}

/*
 * Kernel/runtime state belongs to this interruption frame. Keep one preempt
 * reference across IRQ exit: switching tasks before UIRET could strand the
 * receiver with UIF clear. The interrupted kernel boundary handles NEED_RESCHED
 * after UIRET; FSBASE, SIMD and a partial user-return transition resume exactly.
 */
static __noreturn void uintr_handle_kernel(struct pt_regs *regs,
					 const struct flux_deferred_fault *fault,
					 struct xregs_state *interrupted_xstate,
					 unsigned long interrupted_fsbase,
					 bool restore_in_kernel
#ifdef CONFIG_FLUX_MPK
					 , unsigned int interrupted_pkru
#endif
)
{
	struct pt_regs *old_regs = current->thread.regs;
	irqentry_state_t irq_state;
	bool restore_tsc;

	memset(&interrupted_xstate->header, 0,
	       sizeof(interrupted_xstate->header));
	save_xstate_full(interrupted_xstate);
	restore_tsc = flux_tsc_enter_kernel_mode();

	current->thread.regs = regs;
	preempt_disable();
	irq_state = irqentry_enter(regs);
	flux_uintr_dispatch_vector(regs, regs->uirrv & ~FLUX_UINTR_SYNTHETIC_FLAG);
	irqentry_exit(regs, irq_state);
	preempt_enable_no_resched();
	if (fault->signal) {
		struct flux_kernel_fault_exit context = {
			.fault = *fault,
			.regs = regs,
			.old_regs = old_regs,
			.xstate = interrupted_xstate,
			.fsbase = interrupted_fsbase,
			.restore_in_kernel = restore_in_kernel,
			.restore_tsc = restore_tsc,
		};

		__ret_from_uintr_kernel_fault(&context);
	}
	current->thread.regs = old_regs;

	if (restore_in_kernel)
		this_cpu_write(tls_pcpu.in_kernel, false);
	wrfsbase(interrupted_fsbase);
	flux_tsc_restore_user_mode(restore_tsc);
	BUG_ON(restore_xstate(interrupted_xstate));
	__ret_from_uintr(regs
#ifdef CONFIG_FLUX_MPK
			 , interrupted_pkru
#endif
	);
}

/*
 * Assembly captures GPRs, FSBASE and PKRU with UIF clear. Classify the context
 * using only integer state; each path saves SIMD to its owner before calling
 * any IRQ or host code.
 */
asmlinkage __noreturn void uintr_handler(
	struct pt_regs *regs
#ifdef CONFIG_FLUX_MPK
	, unsigned int interrupted_pkru
#endif
	, struct xregs_state *interrupted_xstate
)
{
	unsigned long interrupted_fsbase = regs->orig_ax;
	bool on_task_stack =
		regs->sp >= (unsigned long)task_stack_page(current) &&
		regs->sp < task_stack_top(current);
	bool on_user_return_path = flux_uintr_on_user_return_path(regs->ip);
	bool from_user, restore_in_kernel;
	struct flux_deferred_fault fault;

	/* Claim before any dispatch can admit nesting; this helper uses only GPRs. */
	flux_take_deferred_fault(&fault);

	/*
	 * Both halves of the protected task stack are kernel context. A return
	 * trampoline can already have cleared in_kernel or loaded APP PKRU while
	 * still needing to resume its kernel-side transition. Preserve that PKRU
	 * snapshot on a user stack; a protected stack always resumes with kernel
	 * PKRU. Host POSIX frames have been consumed before this entry can run.
	 */
#ifdef CONFIG_FLUX_MPK
	if (on_task_stack)
		interrupted_pkru = FLUX_MPK_KERNEL_PKRU;
	else
		BUG_ON(interrupted_pkru != FLUX_MPK_KERNEL_PKRU &&
		       interrupted_pkru != FLUX_MPK_APP_PKRU);
	from_user = !on_task_stack && !on_user_return_path &&
		    interrupted_pkru == FLUX_MPK_APP_PKRU;
#else
	from_user = !on_task_stack && !on_user_return_path &&
		    !this_cpu_read(tls_pcpu.in_kernel);
#endif
	restore_in_kernel = !from_user && !this_cpu_read(tls_pcpu.in_kernel);
	if (restore_in_kernel)
		this_cpu_write(tls_pcpu.in_kernel, true);
	regs->umode = from_user;
	regs->orig_ax = -1;

	if (from_user)
		uintr_handle_user(regs, interrupted_fsbase, &fault);
	uintr_handle_kernel(regs, &fault, interrupted_xstate, interrupted_fsbase,
			    restore_in_kernel
#ifdef CONFIG_FLUX_MPK
			    , interrupted_pkru
#endif
	);
}

void kernel_fpu_begin(void)
{
	preempt_disable();

	if (!test_thread_flag(TIF_NEED_FPU_LOAD) &&
	    test_thread_flag(TIF_USER)) {
		save_xstate_full(task_xstate(current));
		set_thread_flag(TIF_NEED_FPU_LOAD);
	}
}

void kernel_fpu_end(void)
{
	preempt_enable();
}
