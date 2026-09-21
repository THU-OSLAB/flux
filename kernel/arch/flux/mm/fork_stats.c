// SPDX-License-Identifier: GPL-2.0
/* Cumulative elapsed-time observations; no fork or COW policy changes. */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include <asm/fork_stats.h>

struct flux_fork_counters {
	atomic64_t count[FLUX_FORK_PHASES];
	atomic64_t ns[FLUX_FORK_PHASES];
};

bool flux_fork_stats_enabled __read_mostly;

static DEFINE_PER_CPU_SHARED_ALIGNED(struct flux_fork_counters, fork_counters);

static const char *const fork_names[] = {
	[FLUX_FORK_PREPARE] = "prepare",
	[FLUX_FORK_VMA_COPY] = "vma_copy",
	[FLUX_FORK_HOST_COPY] = "host_copy",
	[FLUX_FORK_FINISH] = "finish",
	[FLUX_FORK_UNALIAS] = "unalias",
	[FLUX_FORK_ABORT_COPY] = "abort_copy",
};

void flux_fork_stats_end(enum flux_fork_phase phase, u64 start)
{
	struct flux_fork_counters *counters;
	u64 elapsed;

	if (!start)
		return;
	elapsed = ktime_get_ns() - start;
	/* Nested host signals may observe the same CPU. */
	preempt_disable_notrace();
	counters = raw_cpu_ptr(&fork_counters);
	atomic64_inc(&counters->count[phase]);
	atomic64_add(elapsed, &counters->ns[phase]);
	preempt_enable_no_resched_notrace();
}

static int fork_stats_show(struct seq_file *m, void *unused)
{
	unsigned int phase, cpu;

	seq_printf(m, "version 1\nenabled %u\n",
		   READ_ONCE(flux_fork_stats_enabled));
	for (phase = 0; phase < FLUX_FORK_PHASES; phase++) {
		u64 count = 0, ns = 0;

		for_each_possible_cpu(cpu) {
			struct flux_fork_counters *counters =
				&per_cpu(fork_counters, cpu);

			count += atomic64_read(&counters->count[phase]);
			ns += atomic64_read(&counters->ns[phase]);
		}
		seq_printf(m, "%s %llu %llu\n", fork_names[phase], count, ns);
	}
	return 0;
}

static ssize_t fork_stats_write(struct file *file, const char __user *buf,
				size_t count, loff_t *pos)
{
	char input[32];
	bool enabled;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = 0;
	if (sysfs_streq(input, "enable 1"))
		enabled = true;
	else if (sysfs_streq(input, "enable 0"))
		enabled = false;
	else
		return -EINVAL;
	WRITE_ONCE(flux_fork_stats_enabled, enabled);
	return count;
}

static int fork_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, fork_stats_show, NULL);
}

static const struct proc_ops fork_stats_ops = {
	.proc_open = fork_stats_open,
	.proc_read = seq_read,
	.proc_write = fork_stats_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init flux_fork_stats_init(void)
{
	if (!proc_create("flux_fork_stats", 0600, NULL, &fork_stats_ops))
		return -ENOMEM;
	return 0;
}
late_initcall(flux_fork_stats_init);
