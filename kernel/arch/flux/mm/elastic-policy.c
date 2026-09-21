// SPDX-License-Identifier: GPL-2.0
/* Opt-in, bounded hysteresis policy; allocator fast paths never call the host. */
#include <linux/capability.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/elastic.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>

static DEFINE_MUTEX(policy_lock);
static bool policy_enabled;
static unsigned int low_mb = 256, high_mb = 512, reserve_mb = 4;
static unsigned int interval_ms = 1000;
static unsigned long host_available_mb;
static u64 ticks, shrinks, grows;
static int policy_error;
static void policy_tick(struct work_struct *work);
static DECLARE_DELAYED_WORK(policy_work, policy_tick);

static int host_available(unsigned long *available)
{
	char buffer[512], *line;
	unsigned long kb;
	long fd, count;

	fd = host_syscall(__NR_openat, AT_FDCWD, "/proc/meminfo",
			  O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return fd;
	count = host_syscall(__NR_read, fd, buffer, sizeof(buffer) - 1);
	host_syscall(__NR_close, fd);
	if (count <= 0)
		return count ?: -EIO;
	buffer[count] = 0;
	line = strstr(buffer, "MemAvailable:");
	if (!line || sscanf(line, "MemAvailable: %lu kB", &kb) != 1)
		return -EINVAL;
	*available = kb / 1024;
	return 0;
}

static void policy_tick(struct work_struct *work)
{
	struct flux_elastic_snapshot s;
	unsigned long available;
	unsigned int low = READ_ONCE(low_mb), high = READ_ONCE(high_mb);
	int ret;

	if (!READ_ONCE(policy_enabled))
		return;
	ret = host_available(&available);
	WRITE_ONCE(ticks, ticks + 1);
	if (ret)
		goto out;
	WRITE_ONCE(host_available_mb, available);
	flux_elastic_snapshot(&s);
	if (s.enabled && low < high) {
		if (available < low && s.resident_mb) {
			/* At most one live extent evacuation per tick. Busy extents
			 * stay resident; a later tick can retry without losing data.
			 */
			ret = flux_elastic_resize(s.resident_mb - 2, 1);
			if (!ret)
				WRITE_ONCE(shrinks, shrinks + 1);
		} else if (available > high && s.resident_mb < s.capacity_mb &&
			   s.free_pages < READ_ONCE(reserve_mb) * 256) {
			ret = flux_elastic_resize(s.resident_mb + 2, 0);
			if (!ret)
				WRITE_ONCE(grows, grows + 1);
		}
	}
out:
	WRITE_ONCE(policy_error, ret);
	if (READ_ONCE(policy_enabled))
		queue_delayed_work(system_freezable_wq, &policy_work,
				   msecs_to_jiffies(READ_ONCE(interval_ms)));
}

static int policy_show(struct seq_file *m, void *v)
{
	mutex_lock(&policy_lock);
	seq_printf(
		m,
		"enabled %u\nlow_mb %u\nhigh_mb %u\nreserve_mb %u\ninterval_ms %u\nhost_available_mb %lu\nticks %llu\nshrinks %llu\ngrows %llu\nlast_error %d\n",
		policy_enabled, low_mb, high_mb, reserve_mb, interval_ms,
		READ_ONCE(host_available_mb), READ_ONCE(ticks),
		READ_ONCE(shrinks), READ_ONCE(grows), READ_ONCE(policy_error));
	mutex_unlock(&policy_lock);
	return 0;
}

static ssize_t policy_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *pos)
{
	char input[64], command[24], extra;
	unsigned int value;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = 0;
	if (sscanf(input, "%23s %u %c", command, &value, &extra) != 2)
		return -EINVAL;
	/* The worker never takes policy_lock, so cancel cannot deadlock with it.
	 * Writers stay serialized through cancellation and re-enabling.
	 */
	mutex_lock(&policy_lock);
	if (!strcmp(command, "enable") && value <= 1) {
		WRITE_ONCE(policy_enabled, value);
		if (value)
			mod_delayed_work(system_freezable_wq, &policy_work, 0);
		else
			cancel_delayed_work_sync(&policy_work);
	} else if (!strcmp(command, "low_mb") && value < high_mb) {
		WRITE_ONCE(low_mb, value);
	} else if (!strcmp(command, "high_mb") && value > low_mb &&
		   value <= (1U << 20)) {
		WRITE_ONCE(high_mb, value);
	} else if (!strcmp(command, "reserve_mb") && value &&
		   value <= CONFIG_FLUX_ELASTIC_POOL_MB && !(value % 2)) {
		WRITE_ONCE(reserve_mb, value);
	} else if (!strcmp(command, "interval_ms") && value >= 50 &&
		   value <= 60000) {
		WRITE_ONCE(interval_ms, value);
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&policy_lock);
	return ret ?: count;
}

static int policy_open(struct inode *inode, struct file *file)
{
	return single_open(file, policy_show, NULL);
}

static const struct proc_ops policy_ops = {
	.proc_open = policy_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = policy_write,
};

static int __init policy_init(void)
{
	return proc_create("flux_elastic_policy", 0600, NULL, &policy_ops) ?
		       0 :
		       -ENOMEM;
}
late_initcall(policy_init);
