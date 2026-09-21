// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "mpk_uaccess: " fmt

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/barrier.h>
#include <asm/unistd.h>
#include <asm/host_dev.h>
#include <asm/mpk_uaccess.h>
#include <asm/processor.h>

static struct flux_mpk_base_map *flux_mpk_base_maps;
static unsigned int flux_mpk_base_map_count;

static bool flux_mpk_ranges_compact(struct flux_mpk_base_map *maps,
				    unsigned int *nr)
{
	unsigned int i, out = 0;

	for (i = 0; i < *nr; i++) {
		if (maps[i].start >= maps[i].end)
			return false;
		if (i && maps[i - 1].start > maps[i].start)
			return false;

		if (out && maps[out - 1].end >= maps[i].start) {
			if (maps[i].end > maps[out - 1].end)
				maps[out - 1].end = maps[i].end;
			continue;
		}
		if (out != i)
			maps[out] = maps[i];
		out++;
	}
	*nr = out;
	return true;
}

int flux_mpk_uaccess_init(void)
{
	struct flux_mpk_base_map *maps;
	unsigned int nr = 0, fetched = 0;
	int ret;

	ret = flux_host_dev_get_base_maps(NULL, 0, &nr);
	if (ret != -ENOSPC || !nr) {
		if (!ret)
			ret = -ENOENT;
		return ret;
	}

	maps = kcalloc(nr, sizeof(*maps), GFP_KERNEL);
	if (!maps)
		return -ENOMEM;

	ret = flux_host_dev_get_base_maps(maps, nr, &fetched);
	if (ret || fetched != nr) {
		kfree(maps);
		if (!ret)
			ret = -EAGAIN;
		return ret;
	}
	if (!flux_mpk_ranges_compact(maps, &fetched)) {
		kfree(maps);
		return -EINVAL;
	}

	WRITE_ONCE(flux_mpk_base_maps, maps);
	smp_store_release(&flux_mpk_base_map_count, fetched);
	pr_info("protecting %u pre-application host VMA ranges (%u raw)\n",
		fetched, nr);
	return 0;
}

void flux_mpk_uaccess_enter(void)
{
	current->thread.mpk_uaccess_depth++;
}

void flux_mpk_uaccess_exit(void)
{
	current->thread.mpk_uaccess_depth--;
}

unsigned int flux_mpk_uaccess_suspend(void)
{
	unsigned int depth = current->thread.mpk_uaccess_depth;

	current->thread.mpk_uaccess_depth = 0;
	return depth;
}

void flux_mpk_uaccess_resume(unsigned int depth)
{
	current->thread.mpk_uaccess_depth = depth;
}

void flux_mpk_uaccess_reset(void)
{
	current->thread.mpk_uaccess_depth = 0;
}

bool flux_mpk_uaccess_range_ok(const void __user *addr, unsigned long len)
{
	const struct flux_mpk_base_map *maps;
	unsigned long start = (unsigned long)addr;
	unsigned long end;
	unsigned int lo, hi, nr;

	if (!len)
		return true;
	if (start >= TASK_SIZE || len > TASK_SIZE - start ||
	    check_add_overflow(start, len, &end))
		return false;

	nr = smp_load_acquire(&flux_mpk_base_map_count);
	maps = READ_ONCE(flux_mpk_base_maps);

	lo = 0;
	hi = nr;
	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;

		if (maps[mid].end <= start)
			lo = mid + 1;
		else
			hi = mid;
	}

	return lo == nr || maps[lo].start >= end;
}

bool flux_mpk_uaccess_ok(const void __user *addr, unsigned long len)
{
	if (!current->thread.mpk_uaccess_depth)
		return true;
	return flux_mpk_uaccess_range_ok(addr, len);
}
EXPORT_SYMBOL(flux_mpk_uaccess_ok);
