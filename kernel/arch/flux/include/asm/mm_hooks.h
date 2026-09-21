/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_MM_HOOKS_H
#define _ASM_FLUX_MM_HOOKS_H

#include <asm/fork_stats.h>
#include <asm/host_dev.h>
#include <asm/tlbflush.h>

int flux_init_host_mm(struct mm_struct *mm, bool for_fork);
void flux_release_host_mm(struct mm_struct *mm);

#ifdef CONFIG_FLUX_HOST_FAULT_REPAIR
void flux_prepare_fork_projection(struct mm_struct *mm);
#else
static inline void flux_prepare_fork_projection(struct mm_struct *mm)
{
}
#endif

/*
 * Acquire the parent alias gate with both Flux mmap write locks held, before
 * copy_page_range establishes COW PTEs. The child slot has no host mm yet:
 * failed VMA admission must not copy a potentially huge host reservation tree.
 */
static inline int arch_dup_mmap_prepare(struct mm_struct *oldmm,
					struct mm_struct *mm)
{
	u64 start;
	int ret;

	start = flux_fork_stats_start();
	ret = flux_init_host_mm(mm, true);
	flux_fork_stats_end(FLUX_FORK_PREPARE, start);
#ifdef CONFIG_FLUX_FORK_STATS
	mm->context.fork_copy_start = ret ? 0 : flux_fork_stats_start();
#endif
	return ret;
}
#define arch_dup_mmap_prepare arch_dup_mmap_prepare

static inline int arch_dup_mmap(struct mm_struct *oldmm, struct mm_struct *mm)
{
	u64 start;
	int ret;

	/* All Flux VMAs and PTEs are copied; the gate still protects COW. */
#ifdef CONFIG_FLUX_FORK_STATS
	flux_fork_stats_end(FLUX_FORK_VMA_COPY, mm->context.fork_copy_start);
	mm->context.fork_copy_start = 0;
#endif
	start = flux_fork_stats_start();
	ret = flux_host_dev_fork_alias_begin(mm->context.proc_key);
	if (!ret)
		flux_prepare_fork_projection(mm);
	flux_fork_stats_end(FLUX_FORK_HOST_COPY, start);
	return ret;
}

/*
 * Finish under the parent mmap write lock. A rejected fork releases its slot
 * before exit_mmap: there is no child host mm whose aliases need revocation.
 * On success the gate has already flushed downgraded parent host PTEs, so
 * unchanged aliases survive fork. Other TLB flushes keep normal revocation.
 */
static inline int arch_dup_mmap_finish(struct mm_struct *oldmm,
				       struct mm_struct *mm, int error)
{
	u64 start;
	int ret;

	if (mm->context.proc_key <= 0) {
		flush_tlb_mm(oldmm);
		return error;
	}
	if (error) {
#ifdef CONFIG_FLUX_FORK_STATS
		flux_fork_stats_end(FLUX_FORK_ABORT_COPY,
				    mm->context.fork_copy_start);
		mm->context.fork_copy_start = 0;
#endif
		flux_release_host_mm(mm);
		return error;
	}
	start = flux_fork_stats_start();
	ret = flux_host_dev_fork_alias_end(mm->context.proc_key);
	flux_fork_stats_end(FLUX_FORK_FINISH, start);
	if (ret) {
		flux_release_host_mm(mm);
		flush_tlb_mm(oldmm);
	}
	return ret;
}
#define arch_dup_mmap_finish arch_dup_mmap_finish

static inline void arch_exit_mmap(struct mm_struct *mm)
{
	u64 start = flux_fork_stats_start();

	/* Revoke before unmap_vmas frees pages; kernel projections stay usable. */
	if (mm->context.proc_key > 0)
		BUG_ON(flux_host_dev_unalias_user_mm(mm->context.proc_key));
	flux_fork_stats_end(FLUX_FORK_UNALIAS, start);
}

static inline void arch_unmap(struct mm_struct *mm, unsigned long start,
			      unsigned long end)
{
#ifdef CONFIG_FLUX_HOST_FAULT_REPAIR
	/* Retire admission even when the removed VMA has no present PTEs. */
	flux_flush_tlb_gather(mm, start, end);
#endif
}

static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
					     bool write, bool execute,
					     bool foreign)
{
	return true;
}

#endif /* _ASM_FLUX_MM_HOOKS_H */
