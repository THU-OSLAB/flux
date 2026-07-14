#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/cpu.h>
#include <asm/numa.h>

DEFINE_PER_CPU_SHARED_ALIGNED(struct completion, cpu_running);
DECLARE_EARLY_PER_CPU(bool, boot_flag);

void smp_prepare_cpus(unsigned int max_cpus)
{
	int cpu;

	if (flux_ipi_gate_open())
		panic("flux: failed to open ipi gate\n");

	cpu = smp_processor_id();
	numa_store_cpu_info(cpu);
	numa_add_cpu(cpu);

	if (!max_cpus)
		return;

	for_each_possible_cpu(cpu) {
		if (cpu == smp_processor_id())
			continue;

		set_cpu_present(cpu, true);
		numa_store_cpu_info(cpu);
		init_completion(per_cpu_ptr(&cpu_running, cpu));
	}
}

int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	/* set current task for AP */
	per_cpu(tls_pcpu.current_task, cpu) = idle;
	per_cpu(tls_pcpu.stack_top, cpu) = task_stack_top(idle);
	per_cpu(tls_pcpu.uintr_stack_top, cpu) = uintr_stack_top(idle);

	/* wake up AP pthreads */
	early_per_cpu(boot_flag, cpu) = true;

	/* wait until AP calls smp_callin */
	wait_for_completion_timeout(per_cpu_ptr(&cpu_running, cpu),
				    msecs_to_jiffies(1000));
	if (!cpu_online(cpu)) {
		pr_crit("CPU%u failed to come online\n", cpu);
		return -EIO;
	}
	return 0;
}

/*
 * C entry point for a secondary processor.
 */
void start_secondary(int cpu)
{
	struct mm_struct *mm = &init_mm;

	/* Set up %gs. */
	wrgsbase(per_cpu_offset(cpu));

	/* Cached physical CPU number. */
	this_cpu_write(tls_pcpu.host_tid, (int)flux_ops_gettid_raw());
	BUG_ON(cpu != smp_processor_id());

	/* All kernel threads share the same mm context.  */
	mmgrab(mm);
	current->active_mm = mm;

	notify_cpu_starting(cpu);

	numa_add_cpu(cpu);
	set_cpu_online(cpu, true);

	complete(per_cpu_ptr(&cpu_running, cpu));

	/*
     * Disable preemption before enabling interrupts, so we don't try to
     * schedule a CPU that hasn't actually started yet.
     */
	local_irq_enable();

	/* init local clock */
	flux_cpu_clock_init(cpu);

	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}
