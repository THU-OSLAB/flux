/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_MMU_H
#define _ASM_FLUX_MMU_H

#ifndef __ASSEMBLY__
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/seqlock.h>

struct mm_struct;

typedef struct {
	int proc_key;
#ifdef CONFIG_FLUX_FORK_STATS
	u64 fork_copy_start;
#endif
	unsigned long vdso;
	/* Serializes executable-byte transactions across Linux MM calls. */
	struct mutex host_rewrite_mutex;
	/* Guards record storage, never application memory accesses. */
	struct mutex host_rewrite_state_mutex;
	/* Published only while an executable-byte transaction needs its PTEs. */
	seqlock_t host_rewrite_range_lock;
	unsigned long host_rewrite_start;
	unsigned long host_rewrite_end;
} mm_context_t;

#define INIT_MM_CONTEXT(name) \
	.context.proc_key = 0, \
	.context.host_rewrite_mutex = \
		__MUTEX_INITIALIZER(name.context.host_rewrite_mutex), \
	.context.host_rewrite_state_mutex = \
		__MUTEX_INITIALIZER(name.context.host_rewrite_state_mutex), \
	.context.host_rewrite_range_lock = \
		__SEQLOCK_UNLOCKED(name.context.host_rewrite_range_lock),

bool flux_user_alias_addr_valid(unsigned long addr);
bool flux_user_pte_present(struct mm_struct *mm, unsigned long addr);
bool flux_user_pte_uffd_wp(struct mm_struct *mm, unsigned long addr);
bool flux_repair_present_user_alias_atomic(struct mm_struct *mm,
					   unsigned long addr, unsigned long err);
bool flux_repair_present_user_alias_blocking(struct mm_struct *mm,
					     unsigned long addr);
void flux_ensure_alias(struct mm_struct *mm, unsigned long addr);
int flux_ensure_kernel_vmalloc_alias(unsigned long addr, bool write);

#endif

#endif /* _ASM_FLUX_MMU_H */
