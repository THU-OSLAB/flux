/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FLUX_KMOD_COMPAT_MM_H
#define _FLUX_KMOD_COMPAT_MM_H

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/version.h>

/* follow_pfnmap_* replaced follow_pte in 6.12. Flux aliases are base pages. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
struct flux_follow_pfnmap_args {
	struct vm_area_struct *vma;
	unsigned long address;
	spinlock_t *lock;
	pte_t *ptep;
	unsigned long pfn;
	pgprot_t pgprot;
	bool writable;
};

static inline int flux_follow_pfnmap_start(struct flux_follow_pfnmap_args *args)
{
	pte_t pte;
	int ret;

	if (unlikely(args->address < args->vma->vm_start ||
		     args->address >= args->vma->vm_end))
		return -EINVAL;
	if (!(args->vma->vm_flags & (VM_IO | VM_PFNMAP)))
		return -EINVAL;
	ret = follow_pte(args->vma->vm_mm, args->address, &args->ptep,
			 &args->lock);
	if (ret)
		return ret;
	pte = ptep_get(args->ptep);
	args->pfn = pte_pfn(pte);
	args->pgprot = pte_pgprot(pte);
	args->writable = pte_write(pte);
	return 0;
}

static inline void flux_follow_pfnmap_end(struct flux_follow_pfnmap_args *args)
{
	pte_unmap_unlock(args->ptep, args->lock);
}
#else
#define flux_follow_pfnmap_args follow_pfnmap_args
#define flux_follow_pfnmap_start follow_pfnmap_start
#define flux_follow_pfnmap_end follow_pfnmap_end
#endif

/* get_unmapped_area moved out of mm_struct in Linux 6.12. */
static inline bool flux_compat_mm_area_equal(const struct mm_struct *a,
					     const struct mm_struct *b)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return a->get_unmapped_area == b->get_unmapped_area;
#else
	return true;
#endif
}

typedef int (*flux_host_munmap_fn)(struct mm_struct *, unsigned long, size_t,
				   struct list_head *);

/* Both cache admission and reuse may prune VMAs from an inactive MM.
 * Linux 6.12+ munmap completion accounts against current->mm regardless of
 * the supplied MM. Reject that operation and let the caller discard the
 * candidate through mmput(). Never switch current->mm for cache pruning.
 * The caller holds the target MM's mmap_write_lock().
 */
static inline int flux_compat_prune_vma(flux_host_munmap_fn munmap,
					struct mm_struct *mm,
					unsigned long start, size_t length)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	if (mm != current->mm)
		return -EOPNOTSUPP;
#endif
	if (!munmap)
		return -EOPNOTSUPP;
	return munmap(mm, start, length, NULL);
}

/* Use the host's bulk allocator name; retain its partial-progress result. */
static inline unsigned long flux_compat_alloc_pages_bulk(gfp_t gfp,
							 unsigned long count,
							 struct page **pages)
{
#ifdef alloc_pages_bulk
	return alloc_pages_bulk(gfp, count, pages);
#else
	return alloc_pages_bulk_array(gfp, count, pages);
#endif
}

/* Private host ABI: keep in sync with fs/proc/task_mmu.c::mem_size_stats.
 * The kernels validated for this layout are listed in ../docs/host-compat.md.
 */
struct flux_host_mem_size_stats {
	unsigned long resident;
	unsigned long shared_clean;
	unsigned long shared_dirty;
	unsigned long private_clean;
	unsigned long private_dirty;
	unsigned long referenced;
	unsigned long anonymous;
	unsigned long lazyfree;
	unsigned long anonymous_thp;
	unsigned long shmem_thp;
	unsigned long file_thp;
	unsigned long swap;
	unsigned long shared_hugetlb;
	unsigned long private_hugetlb;
	unsigned long ksm;
	u64 pss;
	u64 pss_anon;
	u64 pss_file;
	u64 pss_shmem;
	u64 pss_dirty;
	u64 pss_locked;
	u64 swap_pss;
};

#endif /* _FLUX_KMOD_COMPAT_MM_H */
