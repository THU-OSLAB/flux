#include <linux/completion.h>
#include <linux/reboot.h>
#include <linux/cpumask.h>
#include <linux/tick.h>
#include <linux/syscalls.h>
#include <linux/compiler.h>
#include <asm/current.h>
#include <asm/smp.h>
#include <asm/host_dev.h>

atomic_t shutdown_cpu = ATOMIC_INIT(-1);
static bool shutdown_cpu_done[NR_CPUS] = { false };
static bool shutdown_cpu_host_mm_done[NR_CPUS] = { false };
extern int is_running;
#if IS_ENABLED(CONFIG_NET_DEV_FNET)
extern void fnet_shutdown(void);
#endif

static __always_inline __noreturn void flux_thread_longjmp_exit(int cpu)
{
	flux_host_call_restore_fsbase(flux_host_fsbase());
	flux_ops->thread_longjmp(cpu);
	unreachable();
}

static void flux_restore_host_mm(int cpu)
{
	int proc_key = current_proc_key();
	int err;

	if (proc_key) {
		err = flux_host_dev_switch_mm(0, proc_key);
		if (unlikely(err)) {
			pr_emerg("flux: failed to restore host mm on CPU%d: proc=%d err=%d\n",
				 cpu, proc_key, err);
			BUG();
		}
		raw_cpu_write(tls_pcpu.current_proc_key, 0);
	}

	/* The leader must not close flux_mm until every CPU is back in proc0. */
	WRITE_ONCE(shutdown_cpu_host_mm_done[cpu], true);
}

void flux_cpu_exit(void)
{
	int cpu = smp_processor_id();

	local_irq_disable();

	WRITE_ONCE(shutdown_cpu_done[cpu], true);

	while (READ_ONCE(is_running))
		cpu_relax();

	flux_restore_host_mm(cpu);
	/* just exit and wait the leader */
	flux_thread_longjmp_exit(cpu);
}

void machine_halt(void)
{
	int cpu = smp_processor_id();
	int i;

	pr_warn("flux: shutting down\n");

	if (atomic_cmpxchg(&shutdown_cpu, -1, cpu) != -1)
		return;

#if IS_ENABLED(CONFIG_NET_DEV_FNET)
	fnet_shutdown();
#endif

	local_irq_disable();

#ifdef CONFIG_SMP
	for_each_online_cpu(i)
		if (i != cpu)
			flux_shutdown(i);
#endif

	WRITE_ONCE(shutdown_cpu_done[cpu], true);

	for_each_online_cpu(i)
		while (!READ_ONCE(shutdown_cpu_done[i]))
			cpu_relax();

	/* Release the APs while the shared mm descriptor is still usable. */
	WRITE_ONCE(is_running, false);
	flux_restore_host_mm(cpu);

	for_each_online_cpu(i)
		while (!READ_ONCE(shutdown_cpu_host_mm_done[i]))
			cpu_relax();

	flux_host_dev_exit();

	flux_thread_longjmp_exit(cpu);
}

void machine_power_off(void)
{
	machine_halt();
}

void machine_restart(char *unused)
{
	machine_halt();
}
