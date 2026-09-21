/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_PGTABLE_H
#define _ASM_FLUX_PGTABLE_H

#include <asm/page.h>

/* ---- Real MMU 4-level page tables (P4D folded, real PUD) ---- */
#include <asm-generic/pgtable-nop4d.h>
#include <asm/pgtable-mmu.h>
#include <asm/processor.h>
#include <asm/io.h>

#ifndef PFN_PTE_SHIFT
#define PFN_PTE_SHIFT		PAGE_SHIFT
#endif

struct mm_struct;
bool flux_rewrite_range_busy(struct mm_struct *mm, unsigned long start,
                             unsigned long end);
#define arch_preserve_user_mapping flux_rewrite_range_busy

int arch_prepare_mmap(unsigned long addr, unsigned long len, unsigned long flags);
#define arch_prepare_mmap arch_prepare_mmap
struct vm_area_struct;
void arch_complete_mmap(struct vm_area_struct *vma);
#define arch_complete_mmap arch_complete_mmap

/* x86 hardware pte flag, only used when writing host page tables. */
#ifndef _PAGE_GLOBAL
#define _PAGE_GLOBAL		0x800
#endif

extern void *empty_zero_page;
void paging_init(void);

static inline void set_pte(pte_t *pteptr, pte_t pteval)
{
	__set_pte(pteptr, pteval);
}

#define pte_flags(pte)		(pte_val(pte) & ~PAGE_MASK)
#define pte_pgprot(pte)		__pgprot(pte_flags(pte))

#define arch_supports_memmap_on_memory arch_supports_memmap_on_memory
static inline bool arch_supports_memmap_on_memory(unsigned long vmemmap_size)
{
	return false;
}

#define ZERO_PAGE(vaddr) virt_to_page(empty_zero_page)


#endif /* _ASM_FLUX_PGTABLE_H */
