#include <linux/seq_file.h>
#include <linux/cpu.h>
#include <asm/x86/cpufeature.h>

// TODO: Functions __generic_xchg_called_with_bad_pointer and wrong_size_cmpxchg
// are called in __generic_xchg and __generic_cmpxchg_local respectively.
// They should be optimized out by the compiler due to the function
// inlining. However, when building with clang there are some instances
// where the functions aren't inlined and, thus, the compile-time optimization
// doesn't eliminate them entirely. As a result, the linker throws
// unresololved symbols error. As a workaround, the fix below define these
// functions to bypass the link-time error.
void __generic_xchg_called_with_bad_pointer(void)
{
	panic("%s shouldn't be executed\n", __func__);
}
unsigned long wrong_size_cmpxchg(volatile void *ptr)
{
	panic("%s shouldn't be executed\n", __func__);
}

#ifdef CONFIG_PROC_FS
static void *cpuinfo_start(struct seq_file *m, loff_t *pos)
{
	if (*pos >= nr_cpu_ids)
		return NULL;

	return (void *)(unsigned long)(*pos + 1);
}

static void *cpuinfo_next(struct seq_file *m, void *v, loff_t *pos)
{
	(*pos)++;
	return cpuinfo_start(m, pos);
}

static void cpuinfo_stop(struct seq_file *m, void *v)
{
}

static int show_cpuinfo(struct seq_file *m, void *v)
{
	unsigned int cpu = (unsigned long)v - 1;
	unsigned int i;

	/*
	 * Expose one conventional record for every Flux virtual CPU.  Flux does
	 * not model SMT or packages separately, so present the virtual CPUs as
	 * distinct cores in one virtual package.
	 */
	seq_printf(m, "processor\t: %u\n", cpu);
	seq_puts(m, "vendor_id\t: Flux\n");
	seq_puts(m, "model name\t: Flux virtual CPU\n");
	seq_puts(m, "physical id\t: 0\n");
	seq_printf(m, "siblings\t: %u\n", nr_cpu_ids);
	seq_printf(m, "core id\t\t: %u\n", cpu);
	seq_printf(m, "cpu cores\t: %u\n", nr_cpu_ids);

	/* All Flux CPUs use the capabilities collected at process startup. */
	seq_puts(m, "flags\t\t:");
	for (i = 0; i < NCAPINTS * 32; i++)
		if (boot_cpu_has(i) && x86_cap_flags[i])
			seq_printf(m, " %s", x86_cap_flags[i]);
	seq_puts(m, "\n\n");

	return 0;
}

const struct seq_operations cpuinfo_op = {
	.start	= cpuinfo_start,
	.next	= cpuinfo_next,
	.stop	= cpuinfo_stop,
	.show	= show_cpuinfo,
};
#endif
