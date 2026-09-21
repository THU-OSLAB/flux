// SPDX-License-Identifier: GPL-2.0
/* Optional observations of existing fault decisions, never fault policy. */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <asm/fault_stats.h>

bool flux_fault_stats_enabled __read_mostly;

struct flux_fault_counters {
	atomic_long_t events[FLUX_FAULT_EVENT_COUNT];
};
static DEFINE_PER_CPU_SHARED_ALIGNED(struct flux_fault_counters,
				     fault_counters);

static const char *const fault_names[FLUX_FAULT_EVENT_COUNT] = {
	[FLUX_FAULT_HOST_USER_READ] = "host_user_read",
	[FLUX_FAULT_HOST_USER_WRITE] = "host_user_write",
	[FLUX_FAULT_HOST_USER_EXEC] = "host_user_exec",
	[FLUX_FAULT_HOST_VMALLOC] = "host_vmalloc",
	[FLUX_FAULT_HOST_OTHER_ADDRESS] = "host_other_address",
	[FLUX_FAULT_HOST_NESTED] = "host_nested",
	[FLUX_FAULT_ATOMIC_NO_STATE] = "atomic_no_state",
	[FLUX_FAULT_ATOMIC_ACCESS_BLOCKED] = "atomic_access_blocked",
	[FLUX_FAULT_ATOMIC_INSTALL_FAILED] = "atomic_install_failed",
	[FLUX_FAULT_ATOMIC_STATE_CHANGED] = "atomic_state_changed",
	[FLUX_FAULT_ATOMIC_REPAIRED] = "atomic_repaired",
	[FLUX_FAULT_BLOCKING_REPAIRED] = "blocking_repaired",
	[FLUX_FAULT_BLOCKING_FAILED] = "blocking_failed",
	[FLUX_FAULT_MM_MISSING_READ] = "mm_missing_read",
	[FLUX_FAULT_MM_MISSING_WRITE] = "mm_missing_write",
	[FLUX_FAULT_MM_MISSING_EXEC] = "mm_missing_exec",
	[FLUX_FAULT_MM_PRESENT_WRITE] = "mm_present_write",
	[FLUX_FAULT_MM_PRESENT_OTHER] = "mm_present_other",
	[FLUX_FAULT_RESOLVER_RETRY] = "resolver_retry",
	[FLUX_FAULT_RESOLVER_ERROR] = "resolver_error",
	[FLUX_FAULT_VMA_DENIED] = "vma_denied",
	[FLUX_FAULT_VMA_MISSING] = "vma_missing",
	[FLUX_FAULT_ATOMIC_GUARD] = "atomic_guard",
};

void __flux_fault_note(enum flux_fault_event event)
{
	/*
	 * Nested host signals can update the same CPU: plain increments lose data.
	 * Restore the entry preemption count without scheduling on a host frame.
	 */
	preempt_disable_notrace();
	atomic_long_inc(&raw_cpu_ptr(&fault_counters)->events[event]);
	preempt_enable_no_resched_notrace();
}

static int fault_stats_show(struct seq_file *m, void *unused)
{
	unsigned int event, cpu;

	seq_printf(m, "version 1\nenabled %u\n",
		   READ_ONCE(flux_fault_stats_enabled));
	for (event = 0; event < FLUX_FAULT_EVENT_COUNT; event++) {
		u64 total = 0;

		for_each_possible_cpu(cpu)
			total += atomic_long_read(
				&per_cpu(fault_counters, cpu).events[event]);
		seq_printf(m, "%s %llu\n", fault_names[event], total);
	}
	return 0;
}

static ssize_t fault_stats_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *pos)
{
	char input[32];
	bool value;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = 0;
	if (sysfs_streq(input, "enable 1"))
		value = true;
	else if (sysfs_streq(input, "enable 0"))
		value = false;
	else
		return -EINVAL;
	/* Cumulative counters are never reset while signal handlers can use them. */
	WRITE_ONCE(flux_fault_stats_enabled, value);
	return count;
}

static int fault_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, fault_stats_show, NULL);
}

static const struct proc_ops fault_stats_ops = {
	.proc_open = fault_stats_open,
	.proc_read = seq_read,
	.proc_write = fault_stats_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init flux_fault_stats_init(void)
{
	if (!proc_create("flux_fault_stats", 0600, NULL, &fault_stats_ops))
		return -ENOMEM;
	return 0;
}
late_initcall(flux_fault_stats_init);
