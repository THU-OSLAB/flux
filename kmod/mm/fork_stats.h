/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FLUX_KMOD_MM_FORK_STATS_H
#define _FLUX_KMOD_MM_FORK_STATS_H

#include <linux/compiler.h>
#include <linux/ktime.h>
#include <linux/types.h>

enum flux_fork_stat {
	/* Timed intervals (inclusive elapsed time). */
	FLUX_FORK_STAT_CACHE_GET,
	FLUX_FORK_STAT_HOST_DUP_MM,
	FLUX_FORK_STAT_ALIAS_GATE,
	FLUX_FORK_STAT_ALIAS_INSTALL,
	FLUX_FORK_STAT_ALIAS_UNALIAS_USER,
	FLUX_FORK_STAT_ALIAS_RELEASE,
	FLUX_FORK_STAT_CACHE_PUT,
	FLUX_FORK_STAT_HOST_MMPUT,

	/* Cache lookup outcomes. */
	FLUX_FORK_STAT_GET_DISABLED,
	FLUX_FORK_STAT_GET_INELIGIBLE,
	FLUX_FORK_STAT_GET_BACKOFF,
	FLUX_FORK_STAT_GET_EMPTY,
	FLUX_FORK_STAT_GET_HIT,
	FLUX_FORK_STAT_GET_REJECT,

	/* First failing candidate validation check. */
	FLUX_FORK_STAT_REJECT_LAYOUT,
	FLUX_FORK_STAT_REJECT_PRUNE,
	FLUX_FORK_STAT_REJECT_VMA_COUNT,
	FLUX_FORK_STAT_REJECT_VMA_RANGE,
	FLUX_FORK_STAT_REJECT_VMA_FLAGS,
	FLUX_FORK_STAT_REJECT_VMA_FILE,
	FLUX_FORK_STAT_REJECT_VMA_OPS,
	FLUX_FORK_STAT_REJECT_VMA_PRIVATE,
	FLUX_FORK_STAT_REJECT_VMA_PROT,
	FLUX_FORK_STAT_REJECT_VMA_SPECIAL,
	FLUX_FORK_STAT_REJECT_VMA_POLICY,
	FLUX_FORK_STAT_REJECT_HUGETLB,
	FLUX_FORK_STAT_REJECT_SHARED,
	FLUX_FORK_STAT_REJECT_PRIVATE_HANDLER,
	FLUX_FORK_STAT_REJECT_ANON_ROOT,
	FLUX_FORK_STAT_REJECT_PRIVATE_SIZE,
	FLUX_FORK_STAT_REJECT_PTE_WALK,
	FLUX_FORK_STAT_REJECT_PTE_PRESENT,
	FLUX_FORK_STAT_REJECT_PTE_WRITE,
	FLUX_FORK_STAT_REJECT_PTE_PFN,
	FLUX_FORK_STAT_REJECT_PTE_PROT,
	FLUX_FORK_STAT_REJECT_PTE_SPECIAL,

	/* Retired-MM cache admission outcomes. */
	FLUX_FORK_STAT_PUT_DISABLED,
	FLUX_FORK_STAT_PUT_INELIGIBLE,
	FLUX_FORK_STAT_PUT_BACKOFF,
	FLUX_FORK_STAT_PUT_ALLOC,
	FLUX_FORK_STAT_PUT_PRIVATE_SIZE,
	FLUX_FORK_STAT_PUT_PRUNE,
	FLUX_FORK_STAT_PUT_BUDGET,
	FLUX_FORK_STAT_PUT_STOPPING,
	FLUX_FORK_STAT_PUT_SAVED,

	FLUX_FORK_STAT_COUNT,
};

extern bool flux_fork_stats_enabled;

void flux_fork_stats_add(enum flux_fork_stat event, u64 ns);

static inline u64 flux_fork_stats_start(void)
{
	if (unlikely(READ_ONCE(flux_fork_stats_enabled)))
		return ktime_get_ns();
	return 0;
}

static inline void flux_fork_stats_end(enum flux_fork_stat event, u64 start)
{
	if (start)
		flux_fork_stats_add(event, ktime_get_ns() - start);
}

static inline void flux_fork_stats_note(enum flux_fork_stat event)
{
	if (unlikely(READ_ONCE(flux_fork_stats_enabled)))
		flux_fork_stats_add(event, 0);
}

#endif /* _FLUX_KMOD_MM_FORK_STATS_H */
