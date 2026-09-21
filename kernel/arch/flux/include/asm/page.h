/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_PAGE_H
#define _ASM_FLUX_PAGE_H

/* Match x86 Linux: data is NX unless the task requests READ_IMPLIES_EXEC. */
#define VM_DATA_DEFAULT_FLAGS VM_DATA_FLAGS_TSK_EXEC

#define ARCH_PFN_OFFSET (memory_start >> PAGE_SHIFT)

/*
 * Real MMU page.h for Flux, derived from arch/um/include/asm/page.h.
 * Flux uses a direct (identity) map: Flux kernel physical address ==
 * kernel virtual address, with PAGE_OFFSET == memory_start (the base of
 * the flux physical memory window, e.g. 0x700000000000). __pa/__va are
 * therefore identities; the buddy allocator hands out real page frames.
 */

#include <linux/const.h>

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE - 1))

#ifdef CONFIG_HUGETLB_PAGE
/* Huge pages use PMD-level 2MB leaves (PMD_SHIFT=21). */
#define HPAGE_SHIFT		21
#define HPAGE_SIZE		(_AC(1, UL) << HPAGE_SHIFT)
#define HPAGE_MASK		(~(HPAGE_SIZE - 1))
#define HUGETLB_PAGE_ORDER	(HPAGE_SHIFT - PAGE_SHIFT)
#endif

#ifndef __ASSEMBLY__

#include <linux/pfn.h>
#include <linux/types.h>

struct page;

#define clear_page(page)	memset((void *)(page), 0, PAGE_SIZE)
#define copy_page(to, from)	memcpy((void *)(to), (void *)(from), PAGE_SIZE)
#define clear_user_page(page, vaddr, pg)	clear_page(page)
#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

/* Four-level, 64-bit (P4D folded): one unsigned long per real level. */
typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pud; } pud_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pgprot; } pgprot_t;
typedef struct page *pgtable_t;

#define pte_val(x)	((x).pte)
#define pmd_val(x)	((x).pmd)
#define pud_val(x)	((x).pud)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)	((x).pgprot)

#define __pte(x)	((pte_t) { (x) })
#define __pmd(x)	((pmd_t) { (x) })
#define __pud(x)	((pud_t) { (x) })
#define __pgd(x)	((pgd_t) { (x) })
#define __pgprot(x)	((pgprot_t) { (x) })

#define pte_get_bits(p, bits)	((p).pte & (bits))
#define pte_set_bits(p, bits)	((p).pte |= (bits))
#define pte_clear_bits(p, bits)	((p).pte &= ~(bits))
#define pte_is_zero(p)		(!(p).pte)
#define pte_set_val(p, phys, prot)	((p).pte = (phys) | pgprot_val(prot))

typedef unsigned long phys_t;

extern unsigned long memory_start, memory_end;

#define PAGE_OFFSET	(memory_start)

#define __va(x)		((void *)((unsigned long)(x)))
#define __pa(x)		((unsigned long)(x))

#define phys_to_pfn(p)	((unsigned long)((p) >> PAGE_SHIFT))
#define pfn_to_phys(pfn)	((phys_t)(pfn) << PAGE_SHIFT)

static inline unsigned long virt_to_pfn(const void *kaddr)
{
	return __pa(kaddr) >> PAGE_SHIFT;
}
#define virt_to_pfn virt_to_pfn

static inline void *pfn_to_virt(unsigned long pfn)
{
	return __va(pfn << PAGE_SHIFT);
}
#define pfn_to_virt pfn_to_virt

/*
 * phys_to_page / virt_to_page / page_to_phys are provided by
 * asm/pgtable-mmu.h (UML-canonical) to avoid duplicate definitions.
 */
#define virt_addr_valid(kaddr)	(((void *)(kaddr) >= (void *)PAGE_OFFSET) && \
				 ((void *)(kaddr) < (void *)memory_end))

void bootmem_init(unsigned long mem_size, unsigned long dma_size);
void misc_mem_init(void);

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#endif /* !__ASSEMBLY__ */


#endif /* _ASM_FLUX_PAGE_H */
