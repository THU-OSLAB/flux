// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched/task.h>
#include <linux/topology.h>
#include <asm/host_ops.h>
#include <asm/sections.h>

#define BOOT_PERCPU_OFFSET ((unsigned long)__per_cpu_load)

DEFINE_PER_CPU_READ_MOSTLY(unsigned long, this_cpu_off) = BOOT_PERCPU_OFFSET;
EXPORT_PER_CPU_SYMBOL(this_cpu_off);

/*
 * Under CONFIG_NUMA the NUMA-aware allocator in arch/flux/mm/numa.c owns
 * __per_cpu_offset and setup_per_cpu_areas (it does the flux percpu
 * finalization too), so compile these out here to avoid duplicate symbols.
 */
#ifndef CONFIG_NUMA
unsigned long __per_cpu_offset[NR_CPUS] __ro_after_init = {
	[0 ... NR_CPUS - 1] = BOOT_PERCPU_OFFSET,
};
EXPORT_SYMBOL(__per_cpu_offset);

void __init setup_per_cpu_areas(void)
{
	unsigned int cpu;
	unsigned long delta;
	int rc;

	/*
	 * Always reserve area for module percpu variables.  That's
	 * what the legacy allocator did.
	 */
	rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
				    PERCPU_DYNAMIC_RESERVE, PAGE_SIZE, NULL,
				    NULL);
	if (rc < 0)
		panic("Failed to initialize percpu areas.");

	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;

	for_each_possible_cpu(cpu) {
		per_cpu_offset(cpu) = delta + pcpu_unit_offsets[cpu];
		per_cpu(this_cpu_off, cpu) = __per_cpu_offset[cpu];
		per_cpu(tls_pcpu.cpu_number, cpu) = cpu;

		if (!cpu) {
			per_cpu(tls_pcpu.host_tid, cpu) = flux_ops_gettid_raw();
			wrgsbase(per_cpu_offset(cpu));
		}
	}
}
#endif /* !CONFIG_NUMA */
