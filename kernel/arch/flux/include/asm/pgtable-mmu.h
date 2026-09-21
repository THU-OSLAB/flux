/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Derived from arch/um/include/asm/pgtable.h
 */
#ifndef _ASM_FLUX_PGTABLE_MMU_H
#define _ASM_FLUX_PGTABLE_MMU_H

#include <asm/page.h>
#include <asm-generic/pgtable-nop4d.h>
#include <linux/mm_types.h>

#ifndef CONFIG_64BIT
#error Not supported bittness for Flux MMU.
#endif

#define _PAGE_PRESENT	0x001
#define _PAGE_RW	0x020
#define _PAGE_USER	0x040
#define _PAGE_ACCESSED	0x080
#define _PAGE_DIRTY	0x100
#define _PAGE_UFFD_WP	0x200	/* userfaultfd write-protected present entry */
#define _PAGE_XOL	0x004	/* logical execution through a task instruction slot */
#define _PAGE_EXEC	0x800	/* executable present PTE; swap uses it as offset */
/* If _PAGE_PRESENT is clear, we use these: */
#define _PAGE_PROTNONE	0x010	/* if the user mapped it with PROT_NONE; pte_present gives true */
#define _PAGE_SWP_UFFD_WP 0x008	/* userfaultfd write-protected swap entry */
#define _PAGE_SWP_EXCLUSIVE	0x400
#define _PAGE_PSE	0x008	/* huge (2MB PMD) page marker for hugetlb */

/* PFN shift within a pte value (flux stores phys addr in the high bits). */
#ifndef PFN_PTE_SHIFT
#define PFN_PTE_SHIFT	PAGE_SHIFT
#endif

/*
 * although we don't distinguish between user space and kernel space
 * reserver half of PGD for user space
 */
#define USER_PTRS_PER_PGD 256
#define FIRST_USER_ADDRESS	0UL


#define PGDIR_SHIFT	39
#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE-1))

#define PUD_SHIFT	30
#define PUD_SIZE	(1UL << PUD_SHIFT)
#define PUD_MASK	(~(PUD_SIZE-1))

#define PMD_SHIFT	21
#define PMD_SIZE	(1UL << PMD_SHIFT)
#define PMD_MASK	(~(PMD_SIZE-1))

/*
 * entries per page directory level
 */
#define PTRS_PER_PTE 512
#define PTRS_PER_PMD 512
#define PTRS_PER_PUD 512
#define PTRS_PER_PGD 512

#define pte_ERROR(e) \
		pr_err("%s:%d: bad pte %p(%016lx).\n", __FILE__, __LINE__, &(e), \
			pte_val(e))
#define pmd_ERROR(e) \
		pr_err("%s:%d: bad pmd %p(%016lx).\n", __FILE__, __LINE__, &(e), \
			pmd_val(e))
#define pud_ERROR(e) \
		pr_err("%s:%d: bad pud %p(%016lx).\n", __FILE__, __LINE__, &(e), \
			pud_val(e))
#define pgd_ERROR(e) \
		pr_err("%s:%d: bad pgd %p(%016lx).\n", __FILE__, __LINE__, &(e), \
			pgd_val(e))

/* P4D is folded into PGD; each entry points at a real PUD page. */
#define p4d_none(x)	(!p4d_val(x))
#define p4d_bad(x)	((p4d_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != \
			 _KERNPG_TABLE)
#define p4d_present(x)	(p4d_val(x) & _PAGE_PRESENT)
#define set_p4d(p4dptr, p4dval) (*(p4dptr) = (p4dval))

static inline void p4d_clear(p4d_t *p4d)
{
	set_p4d(p4d, __p4d(0));
}

#define p4d_page(p4d) phys_to_page(p4d_val(p4d) & PAGE_MASK)
#define p4d_pgtable(p4d) ((pud_t *)__va(p4d_val(p4d) & PAGE_MASK))

#define pud_none(x)	(!pud_val(x))
#define	pud_bad(x)	((pud_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE)
#define pud_present(x)	(pud_val(x) & _PAGE_PRESENT)
#define pud_populate(mm, pud, pmd) \
	set_pud(pud, __pud(_PAGE_TABLE + __pa(pmd)))

#define set_pud(pudptr, pudval) (*(pudptr) = (pudval))

#define set_pmd(pmdptr, pmdval) (*(pmdptr) = (pmdval))

static inline void pud_clear(pud_t *pud)
{
	set_pud(pud, __pud(0));
}

#define pud_page(pud) phys_to_page(pud_val(pud) & PAGE_MASK)
#define pud_pgtable(pud) ((pmd_t *) __va(pud_val(pud) & PAGE_MASK))

static inline unsigned long pte_pfn(pte_t pte)
{
	return phys_to_pfn(pte_val(pte));
}

typedef unsigned long phys_t;

static inline pte_t pfn_pte(unsigned long page_nr, pgprot_t pgprot)
{
	pte_t pte;
	phys_t phys = pfn_to_phys(page_nr);

	pte_set_val(pte, phys, pgprot);
	return pte;
}

static inline pmd_t pfn_pmd(unsigned long page_nr, pgprot_t pgprot)
{
	return __pmd((page_nr << PAGE_SHIFT) | pgprot_val(pgprot));
}

#define set_pmd(pmdptr, pmdval) (*(pmdptr) = (pmdval))

#define pte_pfn(x) phys_to_pfn(pte_val(x))
#define pfn_pte(pfn, prot) __pte(pfn_to_phys(pfn) | pgprot_val(prot))
#define pfn_pmd(pfn, prot) __pmd(pfn_to_phys(pfn) | pgprot_val(prot))

#define pmd_pfn(pmd) (pmd_val(pmd) >> PAGE_SHIFT)

extern pgd_t swapper_pg_dir[PTRS_PER_PGD];

/* Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */

extern unsigned long memory_end;

#define _PAGE_TABLE	(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_ACCESSED | _PAGE_DIRTY)
#define _KERNPG_TABLE	(_PAGE_PRESENT | _PAGE_RW | _PAGE_ACCESSED | _PAGE_DIRTY)
#define _PAGE_CHG_MASK	(PAGE_MASK | _PAGE_ACCESSED | _PAGE_DIRTY | \
			 _PAGE_UFFD_WP)
#define _HPAGE_CHG_MASK	(PAGE_MASK | _PAGE_ACCESSED | _PAGE_DIRTY | \
			 _PAGE_PSE | _PAGE_UFFD_WP)
#define __PAGE_KERNEL_EXEC                                              \
	 (_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED |       \
	  _PAGE_EXEC)
#define PAGE_NONE	__pgprot(_PAGE_PROTNONE | _PAGE_ACCESSED)
#define PAGE_SHARED	__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_COPY	__pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_READONLY	__pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_SHARED_EXEC	__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | \
				 _PAGE_ACCESSED | _PAGE_EXEC)
#define PAGE_COPY_EXEC	__pgprot(_PAGE_PRESENT | _PAGE_USER | \
			       _PAGE_ACCESSED | _PAGE_EXEC)
#define PAGE_READONLY_EXEC __pgprot(_PAGE_PRESENT | _PAGE_USER | \
				    _PAGE_ACCESSED | _PAGE_EXEC)
#define PAGE_KERNEL	__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED)
#define PAGE_KERNEL_EXEC	__pgprot(__PAGE_KERNEL_EXEC)

/*
 * ZERO_PAGE is a global shared page that is always zero: used
 * for zero-mapped memory areas etc..
 */
#define ZERO_PAGE(vaddr) virt_to_page(empty_zero_page)


#define pmd_none(x)	(!pmd_val(x))
#define	pmd_bad(x)	((pmd_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE)

#define pmd_present(x)	(pmd_val(x) & _PAGE_PRESENT)
#define pmd_clear(xp)	do { pmd_val(*(xp)) = 0; } while (0)

#define pmd_page(pmd) phys_to_page(pmd_val(pmd) & PAGE_MASK)

#define pte_page(x) pfn_to_page(pte_pfn(x))

#define pte_present(x)	pte_get_bits(x, (_PAGE_PRESENT | _PAGE_PROTNONE))

void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va, unsigned int nr, pte_t pte);
void munmap_page_for_pte(unsigned long addr, pte_t *xp);

int flux_vmap_pte_range(pte_t *ptep, unsigned long addr, unsigned long end,
		       pgprot_t prot, struct page **pages, int *index);
#define arch_vmap_pte_range flux_vmap_pte_range
void flux_vunmap_pte_range(pte_t *ptep, unsigned long addr, unsigned long end);
#define arch_vunmap_pte_range flux_vunmap_pte_range

static inline void set_pmd_at(struct mm_struct *mm, unsigned long addr,
			      pmd_t *pmdp, pmd_t pmd)
{
	WRITE_ONCE(*pmdp, pmd);
	if ((pmd_val(pmd) & (_PAGE_PRESENT | _PAGE_PSE)) ==
	    (_PAGE_PRESENT | _PAGE_PSE))
		mmap_pages_for_ptes(mm, addr & PMD_MASK, 1,
				     __pte(pmd_val(pmd)));
}

/* Clear the Flux entry without issuing a host operation. */
static inline pte_t flux_pte_clear_raw(pte_t *xp)
{
	pte_t old = READ_ONCE(*xp);
	WRITE_ONCE(*xp, __pte(0));
	return old;
}

static inline void pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *xp)
{
	pte_t old = flux_pte_clear_raw(xp);

	if (pte_present(old))
		munmap_page_for_pte(addr, xp);
}

/*
 * =================================
 * Flags checking section.
 * =================================
 */

static inline int pte_none(pte_t pte)
{
	return pte_is_zero(pte);
}

/*
 * The following only work if pte_present() is true.
 * Undefined behaviour if not..
 */
static inline int pte_read(pte_t pte)
{
	return ((pte_get_bits(pte, _PAGE_USER)) &&
	       !(pte_get_bits(pte, _PAGE_PROTNONE)));
}

static inline int pte_exec(pte_t pte)
{
	return (pte_get_bits(pte, _PAGE_EXEC) &&
	       !(pte_get_bits(pte, _PAGE_PROTNONE)));
}

static inline int pte_write(pte_t pte)
{
	return ((pte_get_bits(pte, _PAGE_RW)) &&
	       !(pte_get_bits(pte, _PAGE_PROTNONE)));
}

static inline int pte_dirty(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_DIRTY);
}

static inline int pte_young(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_ACCESSED);
}

static inline int pmd_write(pmd_t pmd)
{
	return (pmd_val(pmd) & _PAGE_RW) &&
	       !(pmd_val(pmd) & _PAGE_PROTNONE);
}
#define pmd_write pmd_write

static inline int pmd_dirty(pmd_t pmd)
{
	return pmd_val(pmd) & _PAGE_DIRTY;
}

static inline int pmd_young(pmd_t pmd)
{
	return pmd_val(pmd) & _PAGE_ACCESSED;
}
#define pmd_young pmd_young

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline int pmd_trans_huge(pmd_t pmd)
{
	return (pmd_val(pmd) & (_PAGE_PRESENT | _PAGE_PSE)) ==
	       (_PAGE_PRESENT | _PAGE_PSE);
}
#define pmd_trans_huge pmd_trans_huge
#define pmdp_establish generic_pmdp_establish
#endif

static inline pte_t pte_mkclean(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_DIRTY);
	return pte;
}

static inline pte_t pte_mkold(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_ACCESSED);
	return pte;
}

static inline pte_t pte_wrprotect(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_RW);
	return pte;
}

static inline pmd_t pmd_mkold(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) & ~_PAGE_ACCESSED);
}

static inline pmd_t pmd_mkclean(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) & ~_PAGE_DIRTY);
}

static inline pmd_t pmd_mkdirty(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) | _PAGE_DIRTY);
}

static inline pmd_t pmd_mkyoung(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) | _PAGE_ACCESSED);
}

static inline pmd_t pmd_wrprotect(pmd_t pmd)
{
	return __pmd((pmd_val(pmd) & ~_PAGE_RW));
}

static inline pmd_t pmd_mkwrite_novma(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) | _PAGE_RW);
}

static inline pmd_t pmd_mkhuge(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) | _PAGE_PSE);
}

static inline pmd_t pmd_mkinvalid(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) & ~(_PAGE_PRESENT | _PAGE_PROTNONE));
}

#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_WP
static inline int pte_uffd_wp(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_UFFD_WP);
}

static inline pte_t pte_mkuffd_wp(pte_t pte)
{
	pte = pte_wrprotect(pte);
	pte_set_bits(pte, _PAGE_UFFD_WP);
	return pte;
}

static inline pte_t pte_clear_uffd_wp(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_UFFD_WP);
	return pte;
}

static inline int pmd_uffd_wp(pmd_t pmd)
{
	return pmd_val(pmd) & _PAGE_UFFD_WP;
}

static inline pmd_t pmd_mkuffd_wp(pmd_t pmd)
{
	return __pmd(pmd_val(pmd_wrprotect(pmd)) | _PAGE_UFFD_WP);
}

static inline pmd_t pmd_clear_uffd_wp(pmd_t pmd)
{
	return __pmd((pmd_val(pmd) & ~_PAGE_UFFD_WP));
}
#endif

static inline pte_t pte_mkread(pte_t pte)
{
	if (unlikely(pte_get_bits(pte, _PAGE_USER)))
		return pte;
	pte_set_bits(pte, _PAGE_USER);
	return pte;
}

static inline pte_t pte_mkdirty(pte_t pte)
{
	pte_set_bits(pte, _PAGE_DIRTY);
	return pte;
}

static inline pte_t pte_mkyoung(pte_t pte)
{
	pte_set_bits(pte, _PAGE_ACCESSED);
	return pte;
}

static inline pte_t pte_mkwrited(pte_t pte, struct vm_area_struct *vma)
{
	if (unlikely(pte_get_bits(pte,  _PAGE_RW)))
		return pte;
	pte_set_bits(pte, _PAGE_RW);
	return pte;
}

static inline pte_t pte_mkwrite_novma(pte_t pte)
{
	if (unlikely(pte_get_bits(pte,  _PAGE_RW)))
		return pte;
	pte_set_bits(pte, _PAGE_RW);
	return pte;
}

static inline int pte_swp_exclusive(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_SWP_EXCLUSIVE);
}

static inline pte_t pte_swp_mkexclusive(pte_t pte)
{
	pte_set_bits(pte, _PAGE_SWP_EXCLUSIVE);
	return pte;
}

static inline pte_t pte_swp_clear_exclusive(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_SWP_EXCLUSIVE);
	return pte;
}

#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_WP
static inline pte_t pte_swp_mkuffd_wp(pte_t pte)
{
	pte_set_bits(pte, _PAGE_SWP_UFFD_WP);
	return pte;
}

static inline int pte_swp_uffd_wp(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_SWP_UFFD_WP);
}

static inline pte_t pte_swp_clear_uffd_wp(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_SWP_UFFD_WP);
	return pte;
}

static inline pmd_t pmd_swp_mkuffd_wp(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) | _PAGE_SWP_UFFD_WP);
}

static inline int pmd_swp_uffd_wp(pmd_t pmd)
{
	return pmd_val(pmd) & _PAGE_SWP_UFFD_WP;
}

static inline pmd_t pmd_swp_clear_uffd_wp(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) & ~_PAGE_SWP_UFFD_WP);
}
#endif

static inline void update_mmu_cache_range(struct vm_fault *vmf,
		struct vm_area_struct *vma, unsigned long address,
		pte_t *ptep, unsigned int nr)
{
}

static inline void __set_pte(pte_t *pteptr, pte_t pteval)
{
	WRITE_ONCE(*pteptr, pteval);
}

static inline pte_t __pte_next_pfn(pte_t pte)
{
	return __pte(pte_val(pte) + (1UL << PFN_PTE_SHIFT));
}

static inline void __set_ptes(struct mm_struct *mm, unsigned long addr,
		pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned long first_addr = addr;
	unsigned int first_nr = nr;
	pte_t first_pte = pte;

	for (;;) {
		__set_pte(ptep, pte);
		if (--nr == 0)
			break;
		ptep++;
		pte = __pte_next_pfn(pte);
	}

	if (pte_present(first_pte))
		mmap_pages_for_ptes(mm, first_addr, first_nr, first_pte);
}
#define set_ptes(mm, addr, ptep, pte, nr) __set_ptes(mm, addr, ptep, pte, nr)

/*
 * Access-flag faults only make an existing PTE more permissive. set_ptes()
 * synchronizes its active host alias; fault completion repairs an absent
 * projection if that installation could not finish. A spurious-fault flush
 * would revoke the synchronized alias and force the same work again.
 * Page replacement and permission downgrades retain normal TLB revocation.
 */
#define flush_tlb_fix_spurious_fault(vma, address, ptep) do { } while (0)

#define __HAVE_ARCH_PTE_SAME
static inline int pte_same(pte_t pte_a, pte_t pte_b)
{
	return pte_val(pte_a) == pte_val(pte_b);
}

/*
 * hugetlb: a huge page is a PMD-level (2MB) leaf marked _PAGE_PSE. flux is a
 * 4-level table (PGD->PUD->PMD->PTE) with P4D folded; only PMD huge pages are
 * currently supported.
 */
static inline pte_t pte_mkhuge(pte_t pte)
{
	pte_set_bits(pte, _PAGE_PSE);
	return pte;
}

static inline int pte_huge(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_PSE);
}

#ifdef CONFIG_HUGETLB_PAGE
static inline int pmd_huge(pmd_t pmd)
{
	return (pmd_val(pmd) & (_PAGE_PRESENT | _PAGE_PSE)) ==
	       (_PAGE_PRESENT | _PAGE_PSE);
}

static inline int pud_huge(pud_t pud)
{
	return 0;	/* PUD huge pages (1GB) are not supported. */
}

#endif

static inline int pmd_leaf(pmd_t pmd)
{
	return (pmd_val(pmd) & (_PAGE_PRESENT | _PAGE_PSE)) ==
	       (_PAGE_PRESENT | _PAGE_PSE);
}
#define pmd_leaf pmd_leaf

/*
 * Conversion functions: convert a page and protection to a page entry,
 * and a page entry and page directory to the page they refer to.
 */

#define phys_to_page(phys) pfn_to_page(phys_to_pfn(phys))
#define __virt_to_page(virt) phys_to_page(__pa(virt))
#define page_to_phys(page) pfn_to_phys(page_to_pfn(page))
#define virt_to_page(addr) __virt_to_page((const unsigned long) addr)

#define mk_pte(page, pgprot) pfn_pte(page_to_pfn(page), (pgprot))

#define mk_pmd(page, pgprot) pfn_pmd(page_to_pfn(page), (pgprot))

static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	pte_set_val(pte, (pte_val(pte) & _PAGE_CHG_MASK), newprot);
	return pte;
}

static inline pmd_t pmd_modify(pmd_t pmd, pgprot_t newprot)
{
	unsigned long val = pmd_val(pmd);

	val &= _HPAGE_CHG_MASK;
	val |= pgprot_val(newprot) & ~_HPAGE_CHG_MASK;
	return __pmd(val);
}

/*
 * the pmd page can be thought of an array like this: pmd_t[PTRS_PER_PMD]
 *
 * this macro returns the index of the entry in the pmd page which would
 * control the given virtual address
 */
#define pmd_page_vaddr(pmd) ((unsigned long) __va(pmd_val(pmd) & PAGE_MASK))

#define update_mmu_cache(vma, address, ptep) do { } while (0)

static inline void update_mmu_cache_pmd(struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmdp)
{
}

/* Encode and de-code a swap entry */
#define __swp_type(x)			(((x).val >> 5) & 0x1f)
#define __swp_offset(x)			((x).val >> 11)

#define __swp_entry(type, offset) \
	((swp_entry_t) { ((type) << 5) | ((offset) << 11) })
#define __pte_to_swp_entry(pte) \
	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)		((pte_t) { (x).val })

#define HAVE_ARCH_UNMAPPED_AREA

#endif /* _ASM_FLUX_PGTABLE_MMU_H */
