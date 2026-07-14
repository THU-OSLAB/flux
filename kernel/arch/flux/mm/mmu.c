// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/memory.h>
#include <linux/mman.h>
#include <linux/rhashtable.h>
#include <asm/host_ops.h>
#include <asm/page.h>

pgd_t swapper_pg_dir[PTRS_PER_PGD];

static const pgprot_t protection_map[16] = {
	[VM_NONE] = PAGE_NONE,
	[VM_READ] = PAGE_READONLY,
	[VM_WRITE] = PAGE_COPY,
	[VM_WRITE | VM_READ] = PAGE_COPY,
	[VM_EXEC] = PAGE_READONLY,
	[VM_EXEC | VM_READ] = PAGE_READONLY,
	[VM_EXEC | VM_WRITE] = PAGE_COPY,
	[VM_EXEC | VM_WRITE | VM_READ] = PAGE_COPY,
	[VM_SHARED] = PAGE_NONE,
	[VM_SHARED | VM_READ] = PAGE_READONLY,
	[VM_SHARED | VM_WRITE] = PAGE_SHARED,
	[VM_SHARED | VM_WRITE | VM_READ] = PAGE_SHARED,
	[VM_SHARED | VM_EXEC] = PAGE_READONLY,
	[VM_SHARED | VM_EXEC | VM_READ] = PAGE_READONLY,
	[VM_SHARED | VM_EXEC | VM_WRITE] = PAGE_SHARED,
	[VM_SHARED | VM_EXEC | VM_WRITE | VM_READ] = PAGE_SHARED
};
DECLARE_VM_GET_PAGE_PROT

struct va_map_entry {
	unsigned long va;
	unsigned long pa;
	struct rhash_head node;
};
struct rhashtable va_map_table;

static const struct rhashtable_params va_map_params = {
	.key_len = sizeof_field(struct va_map_entry, va),
	.key_offset = offsetof(struct va_map_entry, va),
	.head_offset = offsetof(struct va_map_entry, node),
	.automatic_shrinking = true,
	.min_size = 1,
};

static int __init va_map_init(void)
{
	extern unsigned long mem_size;
	extern unsigned long *mem_blocks;
	size_t i, nblocks = DIV_ROUND_UP(mem_size, MIN_MEMORY_BLOCK_SIZE);
	struct va_map_entry *entry;

	rhashtable_init(&va_map_table, &va_map_params);
	for (i = 0; i < nblocks; ++i) {
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry)
			return -ENOMEM;
		entry->va = mem_blocks[i];
		entry->pa =
			(unsigned long)flux_ops_va_to_pa((void *)mem_blocks[i]);
		rhashtable_insert_fast(&va_map_table, &entry->node,
				       va_map_params);
	}

	pr_info("va_map: %zu entries initialized\n", nblocks);

	return 0;
}
core_initcall(va_map_init);

static unsigned long va_map_lookup(unsigned long va)
{
	struct va_map_entry *entry;
	unsigned long aligned_va = va & ~(MIN_MEMORY_BLOCK_SIZE - 1);
	unsigned long aligned_pa;

	entry = rhashtable_lookup_fast(&va_map_table, &aligned_va,
				       va_map_params);
	aligned_pa = entry ? entry->pa : 0;

	return aligned_pa + (va & (MIN_MEMORY_BLOCK_SIZE - 1));
}

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd = (pgd_t *)__get_free_page(GFP_KERNEL);

	/* no user-space & kernel-space virtual memory boundary for Flux. */
	if (pgd)
		memcpy(pgd, swapper_pg_dir, sizeof(swapper_pg_dir));

	return pgd;
}

static atomic64_t host_pgtable_pool_start = ATOMIC64_INIT(KERNEL_UNMAPPED_BASE);
static unsigned long host_pgtable_pool_end = VMALLOC_START;

static inline struct ptdesc *__pte_alloc_one(struct mm_struct *mm, gfp_t gfp)
{
	struct ptdesc *ptdesc;

	ptdesc = pagetable_alloc(gfp, 0);
	if (!ptdesc)
		return NULL;
	if (!pagetable_pte_ctor(ptdesc)) {
		pagetable_free(ptdesc);
		return NULL;
	}

	return ptdesc;
}

pgtable_t pte_alloc_one(struct mm_struct *mm)
{
	struct ptdesc *ptdesc;

	ptdesc = __pte_alloc_one(mm, GFP_KERNEL | __GFP_ZERO | __GFP_ACCOUNT);
	if (!ptdesc)
		return NULL;

	/* with zero */
	ptdesc->host_pgtable = 0;

	return ptdesc_page(ptdesc);
}

void pte_free(struct mm_struct *mm, struct page *pte_page)
{
	struct ptdesc *ptdesc = page_ptdesc(pte_page);

	if (ptdesc->host_pgtable) {
		/* munmap is not needed because we never touch the address */
		ptdesc->host_pgtable = 0;
	}

	pagetable_pte_dtor(ptdesc);
	pagetable_free(ptdesc);
}

static void host_pgtable_alloc(unsigned long *pa, struct ptdesc *ptdesc)
{
	unsigned long host_pgtable;

	host_pgtable =
		raw_atomic64_fetch_add(PAGE_SIZE, &host_pgtable_pool_start);
	WARN_ON_ONCE(host_pgtable + PAGE_SIZE > host_pgtable_pool_end);

	ptdesc->host_pgtable = host_pgtable;

	*pa = host_pgtable;
}

void mmap_page_for_pte(unsigned long va, pte_t pte, pte_t *ptep)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(ptep);
	unsigned long pa = pte_pfn(pte) << PAGE_SHIFT;
	unsigned long host_pgtable = 0;
	unsigned long host_pa = va_map_lookup(pfn_to_phys(pte_pfn(pte)));
	int prot = PROT_READ;
	int ret;

	flux_debug("mmap_page_for_pte: va %lx, pa %lx\n", va, pa);

	if (ptdesc->host_pgtable) {
		pte_t *host_ptep = (pte_t *)ptdesc->host_pgtable +
				   (ptep - (pte_t *)ptdesc_address(ptdesc));
		pgprot_t pgprot = pte_pgprot(pte);
		pgprot.pgprot |= _PAGE_USER;
		pgprot.pgprot &= ~_PAGE_GLOBAL;
		set_pte(host_ptep, pfn_pte(phys_to_pfn(host_pa), pgprot));
		return;
	}

	/* allocate a host page table for the ptep */
	host_pgtable_alloc(&host_pgtable, ptdesc);

	if (pte_flags(pte) & _PAGE_RW)
		prot |= PROT_WRITE;
	ret = flux_ops_mmap_direct((void *)va, (void *)host_pa, PAGE_SIZE, prot,
				  host_pgtable);
	BUG_ON(ret < 0);
}

void munmap_page_for_pte(unsigned long addr, pte_t *ptep)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(ptep);

	flux_debug("munmap_page_for_pte: addr %lx\n", addr);

	WARN_ON_ONCE(ptdesc->host_pgtable == 0);
	if (ptdesc->host_pgtable) {
		pte_t *host_ptep = (pte_t *)ptdesc->host_pgtable +
				   (ptep - (pte_t *)ptdesc_address(ptdesc));
		WRITE_ONCE(*host_ptep, __pte(0));
		return;
	}
}
