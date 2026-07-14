// SPDX-License-Identifier: GPL-2.0+
#define pr_fmt(fmt) "uintr: " fmt

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/spinlock.h>
#include <linux/entry-common.h>
#include <linux/sched/signal.h>
#include <linux/bitmap.h>
#include <linux/signal.h>
#include <linux/pid.h>
#include <asm/irq_regs.h>
#include <asm/irqflags.h>
#include <asm/processor.h>
#include <asm/x86/fpu.h>
#include <asm/signal.h>
#include <uapi/asm/flux_ops.h>
#include <uapi/asm/flux_oci.h>
#include <uapi/asm/mpk.h>
#include <asm/x86/uintr.h>

extern int uintr_timer_irq;

#define UINTR_IS(name, v) ((v) == (FLUX_UINTR_VECTOR_##name))

struct flux_uipi_pcpu flux_uipi[NR_CPUS] __read_mostly;

#ifdef CONFIG_FLUX_IPI_GATE
struct ipi_gate {
	atomic64_t pending;
} __aligned(L1_CACHE_BYTES) ipi_gates[NR_CPUS];

#endif

static irqreturn_t ipi_handler(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

int flux_ipi_gate_open(void)
{
	int ret;

	ret = request_irq(FLUX_IRQ_IPI, ipi_handler, IRQF_NO_THREAD, "flux_ipi",
			  NULL);
	if (ret < 0) {
		pr_err("failed to register IPI irq %d\n", ret);
		return ret;
	}

#ifdef CONFIG_FLUX_IPI_GATE
	int cpu;

	for_each_possible_cpu(cpu) {
		ipi_gates[cpu].pending.counter = 0;
	}
#endif

	return 0;
}

void init_IRQ(void)
{
	irq_set_chip_and_handler(FLUX_IRQ_IPI, &dummy_irq_chip,
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
#ifdef CONFIG_FLUX_IPI_GATE
void arch_smp_send_reschedule(int cpu)
{
	struct ipi_gate *gate = &ipi_gates[cpu];

	/* Set the pending bit for the target CPU */
	arch_atomic64_or(FLUX_IPI_RESCHED_BIT, &gate->pending);

	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, 0);
}

void arch_send_call_function_single_ipi(int cpu)
{
	struct ipi_gate *gate = &ipi_gates[cpu];

	/* Set the pending bit for the target CPU */
	arch_atomic64_or(FLUX_IPI_CALLFUNC_BIT, &gate->pending);

	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, 0);
}

void flux_tick_broadcast(int cpu)
{
	struct ipi_gate *gate = &ipi_gates[cpu];

	/* Set the pending bit for the target CPU */
	arch_atomic64_or(FLUX_IPI_TICKBC, &gate->pending);

	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, 0);
}

void flux_shutdown(int cpu)
{
	struct ipi_gate *gate = &ipi_gates[cpu];

	/* Set the pending bit for the target CPU */
	arch_atomic64_or(FLUX_IPI_SHUTDOWN_BIT, &gate->pending);

	flux_uintr_send_ipi(raw_smp_processor_id(), cpu, 0);
}

static void handle_ipi(struct pt_regs *regs)
{
	struct ipi_gate *g;
	unsigned long pending = 0;

	g = &ipi_gates[smp_processor_id()];
	pending = arch_atomic64_fetch_and(~FLUX_IPI_MASK, &g->pending);

	if (pending & FLUX_IPI_CALLFUNC_BIT)
		generic_smp_call_function_interrupt();

	if (pending & FLUX_IPI_TICKBC_BIT)
		tick_receive_broadcast();

	if (pending & FLUX_IPI_SHUTDOWN_BIT)
		flux_cpu_exit();

	if (pending & FLUX_IPI_RESCHED_BIT)
		scheduler_ipi();
}

#else
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
#endif /* CONFIG_FLUX_IPI_GATE */
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
	irqentry_state_t state = irqentry_enter(regs);
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

	irqentry_exit(regs, state);
}

static void do_signal_irq(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);
	struct pt_regs *old_regs;
	struct flux_sig_entry *entry;
	struct task_struct *target;
	bool target_from_pid;
	int ret, cpu = raw_smp_processor_id();

	irq_enter_rcu();
	old_regs = set_irq_regs(regs);
	instrumentation_begin();

	/*
	 * Drain as many queued signal entries as possible before return.
	 * The pending bitmap is used as a wakeup hint; actual payload comes
	 * from per-CPU signal queue entries.
	 */
	for (;;) {
		entry = flux_sig_take_entry(cpu);
		if (!entry)
			break;

		target_from_pid = false;
		target = get_pid_task(entry->sig_pid, PIDTYPE_PID);
		if (!target) {
			target = current;
		} else {
			target_from_pid = true;
		}

		if (IS_ENABLED(CONFIG_FLUX_RUNC) &&
		    entry->ctrl_op == FLUX_SIGNAL_CTRL_EXEC) {
			flux_exec_wake();
			goto out_entry;
		}

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
			ret = send_sig(entry->sig_nr, target, 1);
			break;
		}
		if (ret)
			pr_warn("failed to inject signal %d (ret=%d)\n",
				entry->sig_nr, ret);

out_entry:
		if (entry->sig_pid)
			put_pid(entry->sig_pid);
		if (target_from_pid)
			put_task_struct(target);
		kfree(entry);
	}

	instrumentation_end();
	set_irq_regs(old_regs);
	irq_exit_rcu();

	irqentry_exit(regs, state);
}

/**
 * uintr_handler - UINTR handler called from __uintr_handler
 * 
 * UINTR is disabled by User-Interrupt Delivery before entering this
 * function. We don't save fs/gs registers here, as they are used to
 * determine per-CPU states and maintained by the host kernel.
 *
 * @regs: pointer to the uintr_state structure
 */
#ifdef CONFIG_FLUX_MPK
asmlinkage __noreturn void uintr_handler(struct pt_regs *regs,
					 unsigned int interrupted_pkru)
#else
asmlinkage __noreturn void uintr_handler(struct pt_regs *regs)
#endif
{
	u64 uirrv = regs->uirrv;
	bool from_user, temporary_kernel = false;
	struct pt_regs *old_regs = current->thread.regs;
	unsigned long regs_addr = (unsigned long)regs;

	WARN_ON(!arch_irqs_disabled());

#ifdef CONFIG_FLUX_MPK
	if (interrupted_pkru == FLUX_MPK_APP_PKRU) {
		BUG_ON(this_cpu_read(tls_pcpu.in_kernel));
		from_user = true;
	} else if (interrupted_pkru == FLUX_MPK_KERNEL_PKRU) {
		from_user = false;
		if (!this_cpu_read(tls_pcpu.in_kernel)) {
			this_cpu_write(tls_pcpu.in_kernel, true);
			temporary_kernel = true;
		}
	} else {
		BUG();
	}
#else
	from_user = !this_cpu_read(tls_pcpu.in_kernel);
#endif

	if (from_user && regs_addr >= uintr_stack_top(current) &&
	    regs_addr < task_stack_top(current)) {
		this_cpu_write(tls_pcpu.in_kernel, true);
		from_user = false;
		temporary_kernel = true;
	}

	regs->uif = 1;
	regs->umode = from_user;
	regs->orig_ax = -1; /* not a syscall */

	current->thread.regs = regs;

	if (from_user)
		set_thread_flag(TIF_UINTR_FROM_USER);

	if (UINTR_IS(TIMER, uirrv))
		do_irq(regs, uintr_timer_irq);
	else if (UINTR_IS(SIGNAL, uirrv))
		do_signal_irq(regs);
	else
		do_irq(regs, FLUX_IRQ_IPI);

	/*
	 * Do not clear this flag in nested interrupts, otherwise task switch
	 * handling may miss the required FPU/SIMD state processing.
	 */
	WARN_ON(!arch_irqs_disabled());
	if (from_user)
		clear_thread_flag(TIF_UINTR_FROM_USER);

	current->thread.regs = old_regs;

	if (temporary_kernel)
		this_cpu_write(tls_pcpu.in_kernel, false);

#ifdef CONFIG_FLUX_MPK
	__ret_from_uintr(regs, interrupted_pkru);
#else
	__ret_from_uintr(regs);
#endif
}

#ifdef CONFIG_X86_DEBUG_FPU
#define WARN_ON_FPU(x) WARN_ON_ONCE(x)
#else
#define WARN_ON_FPU(x)     \
	({                 \
		(void)(x); \
		0;         \
	})
#endif

void kernel_fpu_begin(void)
{
	preempt_disable();

	if (!test_thread_flag(TIF_NEED_FPU_LOAD) &&
	    test_thread_flag(TIF_UINTR_FROM_USER)) {
		save_xstate(task_xstate(current));
		set_thread_flag(TIF_NEED_FPU_LOAD);
	}
}

void kernel_fpu_end(void)
{
	preempt_enable();
}
