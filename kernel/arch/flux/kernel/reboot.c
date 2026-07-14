#include <linux/completion.h>
#include <linux/reboot.h>
#include <linux/cpumask.h>
#include <linux/tick.h>
#include <linux/syscalls.h>
#include <asm/smp.h>
#include <asm/host_dev.h>

atomic_t shutdown_cpu = ATOMIC_INIT(-1);
static bool shutdown_cpu_done[NR_CPUS] = { false };
extern int is_running;

void flux_cpu_exit(void)
{
	int cpu = smp_processor_id();

	local_irq_disable();

	shutdown_cpu_done[cpu] = true;

	while (is_running)
		cpu_relax();

	/* just exit and wait the leader */
	flux_ops_thread_longjmp(cpu);
}

void machine_halt(void)
{
	int cpu = smp_processor_id();
	int i;

	pr_warn("flux: shutting down\n");

	if (atomic_cmpxchg(&shutdown_cpu, -1, cpu) != -1)
		return;

	local_irq_disable();

#ifdef CONFIG_SMP
	for (i = 0; i < NR_CPUS; i++)
		if (i != cpu)
			flux_shutdown(i);
#endif

	shutdown_cpu_done[cpu] = true;
	for (i = 0; i < NR_CPUS; i++)
		while (!shutdown_cpu_done[i])
			cpu_relax();

	flux_host_dev_exit();

	is_running = false;

	flux_ops_thread_longjmp(cpu);
}

void machine_power_off(void)
{
	machine_halt();
}

void machine_restart(char *unused)
{
	machine_halt();
}
