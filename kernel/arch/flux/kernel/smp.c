#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/rcupdate.h>
#include <linux/sched/stat.h>
#include <linux/sched/debug.h>
#include <linux/sched/mm.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <asm/host_ops.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/syscalls.h>
#include <asm/current.h>
#include <asm/smp.h>
#include <uapi/asm/flux_ops.h>

DEFINE_PER_CPU(uint64_t[2 * FLUX_NR_STATS], pcpu_stat);

struct flux_bind_args {
	int cpu;
	void (*fn)(int);
	void *fn_arg;
};

void flux_bind_wrapper(void *arg)
{
	struct flux_bind_args *args = arg;
	int cpu = args->cpu;

	if (flux_ops_thread_bind(cpu))
		goto out;

	if (args->fn)
		args->fn(cpu);

out:
	flux_ops_mem_free(args);
	flux_ops_thread_exit();
}

flux_thread_t flux_run_on_cpu(int cpu, void (*fn)(int), void *arg)
{
	struct flux_bind_args *args;

	args = flux_ops_mem_alloc(sizeof(struct flux_bind_args));
	if (!args)
		return 0;

	args->fn = fn;
	args->cpu = cpu;
	args->fn_arg = arg;

	return flux_ops_thread_create(flux_bind_wrapper, args, NULL);
}

#ifndef CONFIG_FLUX_UINTR
struct flux_ipi_gate {
	struct flux_mutex *lock;
	struct flux_sem *sched;
	flux_thread_t thread;
	unsigned long pending;
} flux_ipi_gate[NR_CPUS];

void flux_ipi_gate_close(int last);

static irqreturn_t ipi_handler(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static void flux_ipi_thread(int cpu)
{
	long pending;
	struct flux_ipi_gate *g;

	g = &flux_ipi_gate[cpu];

	while (1) {
		flux_ops_sem_down(g->sched);

		pending = g->pending;
		if (pending & FLUX_IPI_EXIT)
			break;

		flux_set_remote_irq_pending(cpu, FLUX_IRQ_IPI);
	}
}

/**
 * flux_ipi_gate_open - open the IPI gate
 */
int flux_ipi_gate_open(void)
{
	long cpu, ret;
	struct flux_ipi_gate *g;

	ret = request_irq(FLUX_IRQ_IPI, ipi_handler, IRQF_NO_THREAD, "flux_ipi",
			  NULL);
	if (ret)
		return ret;

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		g = &flux_ipi_gate[cpu];
		memset(g, 0, sizeof(struct flux_ipi_gate));
		g->lock = flux_ops_mutex_alloc(0);
		g->sched = flux_ops_sem_alloc(0);
		if (g->lock && g->sched)
			g->thread = flux_run_on_cpu(cpu, flux_ipi_thread, NULL);
		if (!g->lock || !g->sched || !g->thread)
			break;
	}

	if (cpu >= NR_CPUS)
		return 0;

	flux_ipi_gate_close(cpu);
	return -ENODEV;
}

void flux_ipi_gate_close(int last)
{
	long cpu;
	struct flux_ipi_gate *g;

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		g = &flux_ipi_gate[cpu];

		if (g->thread) {
			flux_ops_mutex_lock(g->lock);
			g->pending |= FLUX_IPI_EXIT;
			flux_ops_mutex_unlock(g->lock);

			flux_ops_sem_up(g->sched);
			flux_ops_thread_join(g->thread);
		}
		if (g->lock)
			flux_ops_mutex_free(g->lock);
		if (g->sched)
			flux_ops_sem_free(g->sched);
		if (!g->lock && !g->sched && !g->thread)
			break;
	}
	free_irq(FLUX_IRQ_IPI, NULL);
}

static __always_inline void wake_up_ipi_thread(int cpu, flux_ipi_type code)
{
	struct flux_ipi_gate *g;

	if (code & ~FLUX_IPI_MASK)
		return;

	g = &flux_ipi_gate[cpu];

	flux_ops_mutex_lock(g->lock);
	g->pending |= code;
	flux_ops_mutex_unlock(g->lock);

	flux_ops_sem_up(g->sched);
}

void arch_smp_send_reschedule(int cpu)
{
	wake_up_ipi_thread(cpu, FLUX_IPI_RESCHED);
}

void arch_send_call_function_single_ipi(int cpu)
{
	wake_up_ipi_thread(cpu, FLUX_IPI_CALLFUNC);
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	int cpu;

	for_each_cpu(cpu, mask) {
		arch_send_call_function_single_ipi(cpu);
	}
}

/**
 * flux_ipi - handle call function and tick broadcast IPIs. This needs
 * to be called in irq_enter()/irq_exit() pair.
 */
void flux_ipi(void)
{
	struct flux_ipi_gate *g;
	long pending;

	g = &flux_ipi_gate[smp_processor_id()];
	if (!g->lock)
		return;

	flux_ops_mutex_lock(g->lock);
	pending = g->pending;
	g->pending &= ~(FLUX_IPI_CALLFUNC | FLUX_IPI_TICKBC);
	flux_ops_mutex_unlock(g->lock);

	if (pending & FLUX_IPI_CALLFUNC) {
		generic_smp_call_function_interrupt();
	}

	if (pending & FLUX_IPI_TICKBC) {
		tick_receive_broadcast();
	}
}

/**
 * flux_scheduler_ipi - handle scheduler IPIs. This needs to be
 * called out of irq_enter()/irq_exit() pair.
 */
void flux_scheduler_ipi(void)
{
	struct flux_ipi_gate *g;
	long pending;

	g = &flux_ipi_gate[smp_processor_id()];
	if (!g->lock)
		return;

	flux_ops_mutex_lock(g->lock);
	pending = g->pending;
	g->pending &= ~FLUX_IPI_RESCHED;
	flux_ops_mutex_unlock(g->lock);

	if (pending & FLUX_IPI_RESCHED) {
		scheduler_ipi();
	}

	if (pending & FLUX_IPI_SHUTDOWN) {
		flux_cpu_exit();
	}
}

/**
 * flux_tick_broadcast - send tick broadcast IPI to cpu
 * @cpu: cpu to send the IPI to
 */
void flux_tick_broadcast(int cpu)
{
	wake_up_ipi_thread(cpu, FLUX_IPI_TICKBC);
}

void flux_shutdown(int cpu)
{
	wake_up_ipi_thread(cpu, FLUX_IPI_SHUTDOWN);
}
#endif /* CONFIG_FLUX_UINTR */