/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Flux page-table allocation, derived from arch/um/include/asm/pgalloc.h.
 * lower-level allocators come from asm-generic; mmu.c provides pgd_alloc.
 */
#ifndef _ASM_FLUX_PGALLOC_H
#define _ASM_FLUX_PGALLOC_H

#include <linux/mm.h>

#include <asm-generic/pgalloc.h>

#define p4d_populate(mm, p4d, pud) \
	set_p4d(p4d, __p4d(_PAGE_TABLE + (unsigned long)__pa(pud)))

#define pmd_populate_kernel(mm, pmd, pte) \
	set_pmd(pmd, __pmd(_PAGE_TABLE + (unsigned long) __pa(pte)))

#define pmd_populate(mm, pmd, pte)				\
	set_pmd(pmd, __pmd(_PAGE_TABLE +			\
		((unsigned long long)page_to_pfn(pte) <<	\
			(unsigned long long) PAGE_SHIFT)))

/* Provided by arch/flux/mm/mmu.c. */
extern pgd_t *pgd_alloc(struct mm_struct *);

#define __pte_free_tlb(tlb, pte, address)			\
do {								\
	pagetable_pte_dtor(page_ptdesc(pte));			\
	tlb_remove_page_ptdesc((tlb), (page_ptdesc(pte)));	\
} while (0)

/* P4D is folded; PUD, PMD and PTE are real allocated levels. */
#define __pud_free_tlb(tlb, pud, address) pud_free((tlb)->mm, pud)

#define __pmd_free_tlb(tlb, pmd, address)			\
do {								\
	pagetable_pmd_dtor(virt_to_ptdesc(pmd));		\
	tlb_remove_page_ptdesc((tlb), virt_to_ptdesc(pmd));	\
} while (0)

#endif /* _ASM_FLUX_PGALLOC_H */
