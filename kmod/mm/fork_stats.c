// SPDX-License-Identifier: GPL-2.0
/* Optional observations of host fork work; never MM admission policy. */
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/percpu.h>

#include "fork_stats.h"

struct flux_fork_counters {
	atomic64_t count[FLUX_FORK_STAT_COUNT];
	atomic64_t ns[FLUX_FORK_STAT_COUNT];
};

bool flux_fork_stats_enabled;
module_param_named(fork_cost_stats, flux_fork_stats_enabled, bool, 0444);
MODULE_PARM_DESC(fork_cost_stats,
		 "Collect fork phase elapsed time and cache rejection reasons");

static DEFINE_PER_CPU(struct flux_fork_counters, fork_counters);

static const char *const fork_stat_names[FLUX_FORK_STAT_COUNT] = {
	/* Timed intervals (inclusive elapsed time). */
	[FLUX_FORK_STAT_CACHE_GET] = "cache_get",
	[FLUX_FORK_STAT_HOST_DUP_MM] = "host_dup_mm",
	[FLUX_FORK_STAT_ALIAS_GATE] = "alias_gate",
	[FLUX_FORK_STAT_ALIAS_INSTALL] = "alias_install",
	[FLUX_FORK_STAT_ALIAS_UNALIAS_USER] = "alias_unalias_user",
	[FLUX_FORK_STAT_ALIAS_RELEASE] = "alias_release",
	[FLUX_FORK_STAT_CACHE_PUT] = "cache_put",
	[FLUX_FORK_STAT_HOST_MMPUT] = "host_mmput",

	/* Cache lookup outcomes. */
	[FLUX_FORK_STAT_GET_DISABLED] = "get_disabled",
	[FLUX_FORK_STAT_GET_INELIGIBLE] = "get_ineligible",
	[FLUX_FORK_STAT_GET_BACKOFF] = "get_backoff",
	[FLUX_FORK_STAT_GET_EMPTY] = "get_empty",
	[FLUX_FORK_STAT_GET_HIT] = "get_hit",
	[FLUX_FORK_STAT_GET_REJECT] = "get_reject",

	/* First failing candidate validation check. */
	[FLUX_FORK_STAT_REJECT_LAYOUT] = "reject_layout",
	[FLUX_FORK_STAT_REJECT_PRUNE] = "reject_prune",
	[FLUX_FORK_STAT_REJECT_VMA_COUNT] = "reject_vma_count",
	[FLUX_FORK_STAT_REJECT_VMA_RANGE] = "reject_vma_range",
	[FLUX_FORK_STAT_REJECT_VMA_FLAGS] = "reject_vma_flags",
	[FLUX_FORK_STAT_REJECT_VMA_FILE] = "reject_vma_file",
	[FLUX_FORK_STAT_REJECT_VMA_OPS] = "reject_vma_ops",
	[FLUX_FORK_STAT_REJECT_VMA_PRIVATE] = "reject_vma_private",
	[FLUX_FORK_STAT_REJECT_VMA_PROT] = "reject_vma_prot",
	[FLUX_FORK_STAT_REJECT_VMA_SPECIAL] = "reject_vma_special",
	[FLUX_FORK_STAT_REJECT_VMA_POLICY] = "reject_vma_policy",
	[FLUX_FORK_STAT_REJECT_HUGETLB] = "reject_hugetlb",
	[FLUX_FORK_STAT_REJECT_SHARED] = "reject_shared",
	[FLUX_FORK_STAT_REJECT_PRIVATE_HANDLER] = "reject_private_handler",
	[FLUX_FORK_STAT_REJECT_ANON_ROOT] = "reject_anon_root",
	[FLUX_FORK_STAT_REJECT_PRIVATE_SIZE] = "reject_private_size",
	[FLUX_FORK_STAT_REJECT_PTE_WALK] = "reject_pte_walk",
	[FLUX_FORK_STAT_REJECT_PTE_PRESENT] = "reject_pte_present",
	[FLUX_FORK_STAT_REJECT_PTE_WRITE] = "reject_pte_write",
	[FLUX_FORK_STAT_REJECT_PTE_PFN] = "reject_pte_pfn",
	[FLUX_FORK_STAT_REJECT_PTE_PROT] = "reject_pte_prot",
	[FLUX_FORK_STAT_REJECT_PTE_SPECIAL] = "reject_pte_special",

	/* Retired-MM cache admission outcomes. */
	[FLUX_FORK_STAT_PUT_DISABLED] = "put_disabled",
	[FLUX_FORK_STAT_PUT_INELIGIBLE] = "put_ineligible",
	[FLUX_FORK_STAT_PUT_BACKOFF] = "put_backoff",
	[FLUX_FORK_STAT_PUT_ALLOC] = "put_alloc",
	[FLUX_FORK_STAT_PUT_PRIVATE_SIZE] = "put_private_size",
	[FLUX_FORK_STAT_PUT_PRUNE] = "put_prune",
	[FLUX_FORK_STAT_PUT_BUDGET] = "put_budget",
	[FLUX_FORK_STAT_PUT_STOPPING] = "put_stopping",
	[FLUX_FORK_STAT_PUT_SAVED] = "put_saved",
};

void flux_fork_stats_add(enum flux_fork_stat event, u64 ns)
{
	struct flux_fork_counters *counters = get_cpu_ptr(&fork_counters);

	atomic64_inc(&counters->count[event]);
	atomic64_add(ns, &counters->ns[event]);
	put_cpu_ptr(&fork_counters);
}

static int fork_stats_get(char *buffer, const struct kernel_param *kp)
{
	unsigned int event, cpu;
	int len;

	len = scnprintf(buffer, PAGE_SIZE, "version 1\nenabled %u\n",
			flux_fork_stats_enabled);
	for (event = 0; event < FLUX_FORK_STAT_COUNT; event++) {
		u64 count = 0, ns = 0;

		for_each_possible_cpu(cpu) {
			struct flux_fork_counters *counters =
				&per_cpu(fork_counters, cpu);

			count += atomic64_read(&counters->count[event]);
			ns += atomic64_read(&counters->ns[event]);
		}
		len += scnprintf(buffer + len, PAGE_SIZE - len,
				 "%s %llu %llu\n", fork_stat_names[event],
				 count, ns);
	}
	return len;
}

static const struct kernel_param_ops fork_stats_ops = {
	.get = fork_stats_get,
};
module_param_cb(fork_cost_report, &fork_stats_ops, NULL, 0400);
