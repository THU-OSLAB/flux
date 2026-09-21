/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_HUGETLB_H
#define _ASM_FLUX_HUGETLB_H

#include <asm/page.h>
#include <asm-generic/hugetlb.h>

/*
 * Flux hugetlb support. ARCH_WANT_GENERAL_HUGETLB provides the generic
 * huge_pte_alloc()/huge_pte_offset() walkers, and asm-generic/hugetlb.h
 * provides the huge_pte_* accessors as thin wrappers over the normal
 * pte helpers (set_huge_pte_at -> set_pte_at, huge_ptep_get -> ptep_get,
 * etc.). Only the arch predicates below need supplying.
 */

static inline int is_hugepage_only_range(struct mm_struct *mm,
					 unsigned long addr,
					 unsigned long len)
{
	return 0;
}
#define is_hugepage_only_range is_hugepage_only_range

static inline void arch_clear_hugepage_flags(struct page *page)
{
}
#define arch_clear_hugepage_flags arch_clear_hugepage_flags

#endif /* _ASM_FLUX_HUGETLB_H */
