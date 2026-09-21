/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_FORK_STATS_H
#define _ASM_FLUX_FORK_STATS_H

#include <linux/compiler.h>
#include <linux/ktime.h>
#include <linux/types.h>

enum flux_fork_phase {
	FLUX_FORK_PREPARE,
	FLUX_FORK_VMA_COPY,
	FLUX_FORK_HOST_COPY,
	FLUX_FORK_FINISH,
	FLUX_FORK_UNALIAS,
	FLUX_FORK_ABORT_COPY,
	FLUX_FORK_PHASES,
};

#ifdef CONFIG_FLUX_FORK_STATS
extern bool flux_fork_stats_enabled;

void flux_fork_stats_end(enum flux_fork_phase phase, u64 start);

static inline u64 flux_fork_stats_start(void)
{
	if (unlikely(READ_ONCE(flux_fork_stats_enabled)))
		return ktime_get_ns();
	return 0;
}
#else
static inline u64 flux_fork_stats_start(void)
{
	return 0;
}

static inline void flux_fork_stats_end(enum flux_fork_phase phase, u64 start)
{
}
#endif

#endif /* _ASM_FLUX_FORK_STATS_H */
