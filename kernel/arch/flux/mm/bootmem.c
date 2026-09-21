// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/sizes.h>
#include <linux/swap.h>
#include <asm/host_ops.h>
#include <asm/spdk.h>
#include <asm/numa.h>
#ifdef CONFIG_FLUX_STATIC_HUGETLB_SHARE
#include <asm/host_dev.h>
#endif
#ifdef CONFIG_FLUX_ELASTIC_MEMORY
#include <asm/elastic.h>
#endif

unsigned long memory_start, memory_end;

void *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

unsigned long mem_size;
unsigned long mem_block_size;
unsigned long *mem_blocks;
static unsigned long dma_mem_size;

#ifdef CONFIG_FLUX_STATIC_HUGETLB_SHARE
/* 0: base pages, 1: 512 MiB hugetlb VMAs, 2: shareable 1 GiB VMAs. */
static unsigned int static_hugetlb = 2;

static int __init setup_static_hugetlb(char *str)
{
	unsigned int value;

	if (kstrtouint(str, 0, &value) || value > 2)
		return -EINVAL;
	static_hugetlb = value;
	return 0;
}
early_param("static_hugetlb", setup_static_hugetlb);
#endif

static inline void update_max_low_pfn(phys_addr_t end)
{
	unsigned long end_pfn = end >> PAGE_SHIFT;

	if (end_pfn > max_low_pfn)
		max_low_pfn = end_pfn;
}

static unsigned long __init bootmem_node_chunk(unsigned long size,
					       unsigned long unit, int nid,
					       int nr_nodes)
{
	u64 total_units = size / unit;
	u64 base_units = total_units / nr_nodes;
	u64 extra_units = total_units % nr_nodes;
	u64 chunk_units = base_units + (nid < extra_units);

	return (unsigned long)chunk_units * unit;
}

static void __init __maybe_unused
bootmem_alloc_region(unsigned long *base, unsigned long size,
		     unsigned long alloc_align, unsigned long split_align,
		     unsigned int alloc_flags)
{
	int nid, nr_nodes;

#ifdef CONFIG_NUMA
	nr_nodes = flux_max_nodes;
#else
	nr_nodes = 1;
#endif
	for (nid = 0; nid < nr_nodes; nid++) {
		unsigned long chunk_size;
		phys_addr_t chunk_start;
		unsigned long chunk_base = *base;

		chunk_size =
			bootmem_node_chunk(size, split_align, nid, nr_nodes);
		if (!chunk_size)
			continue;

		chunk_start = (phys_addr_t)flux_ops_page_alloc(
			(void *)chunk_base, chunk_size, alloc_align,
			FLUX_PAGE_ALLOC_MAKE_FLAGS(nid, alloc_flags));
		if (!chunk_start || chunk_start != chunk_base) {
			flux_debug("failed to allocate memory\n");
			flux_ops_panic();
		}

		memblock_add_node(chunk_start, chunk_size, nid, 0);
		*base += chunk_size;
	}
}

void __init bootmem_init(unsigned long mem_sz, unsigned long dma_sz)
{
	unsigned long base_addr, nblocks, normal_mem_size;
	int i;

	mem_block_size = MIN_MEMORY_BLOCK_SIZE;
	dma_mem_size = ALIGN(dma_sz, mem_block_size);
	normal_mem_size = ALIGN(mem_sz, PAGE_SIZE);
	mem_size = dma_mem_size + normal_mem_size;
	memory_start = FLUX_MEMORY_ADDR;
	memory_end = memory_start + mem_size;
	nblocks = DIV_ROUND_UP(mem_size, mem_block_size);
	mem_blocks = flux_ops_mem_alloc(nblocks * sizeof(*mem_blocks));
	if (!mem_blocks) {
		flux_debug("failed to allocate mem_blocks\n");
		flux_ops_panic();
	}

	base_addr = memory_start;
#ifdef CONFIG_FLUX_STATIC_HUGETLB_SHARE
	{
		unsigned long pool_size = 0, static_size, left, chunk;
		phys_addr_t allocated;

#ifdef CONFIG_FLUX_ELASTIC_MEMORY
		pool_size = CONFIG_FLUX_ELASTIC_POOL_MB * SZ_1M;
		if (pool_size % SZ_2M || normal_mem_size < pool_size + SZ_64M)
			panic("invalid Flux elastic pool layout");
#endif
		if (static_hugetlb == 2) {
			long features = flux_host_dev_call_mm(FLUX_DEV_IO_MM_FEATURES, 0);

			if (features < 0 || !(features & FLUX_MM_FEATURE_HUGETLB_PMD_SHARE)) {
				flux_debug("host hugetlb PMD sharing is unavailable\n");
				flux_ops_panic();
			}
		}
		static_size = mem_size - pool_size;
		if (!static_size || static_size % SZ_1G || memory_start % SZ_1G)
			panic("Flux static hugetlb memory must be 1 GiB aligned");
		left = static_size;
		while (left) {
			chunk = static_hugetlb == 1 ? SZ_512M : left;
			allocated = (phys_addr_t)flux_ops_page_alloc((void *)base_addr,
				chunk, static_hugetlb ? SZ_2M : PAGE_SIZE,
				static_hugetlb ? FLUX_PAGE_ALLOC_STATIC_HUGE : 0);
			if (allocated != base_addr)
				panic("Flux static memory allocation failed");
			memblock_add_node(base_addr, chunk, 0, 0);
			base_addr += chunk;
			left -= chunk;
		}
		pr_info("Flux static memory: mode %u, %lu MiB\n",
			static_hugetlb, static_size >> 20);
#ifdef CONFIG_FLUX_ELASTIC_MEMORY
		flux_elastic_bootmem(base_addr, pool_size);
		memblock_add_node(base_addr, pool_size, 0, 0);
		memblock_reserve(base_addr, pool_size);
		base_addr += pool_size;
#endif
	}
#else
	bootmem_alloc_region(&base_addr, dma_mem_size, mem_block_size,
			     mem_block_size, FLUX_PAGE_ALLOC_DMA);
#ifdef CONFIG_FLUX_ELASTIC_MEMORY
	/* Reserve once; these pages never enter buddy or its per-CPU caches. */
	{
		unsigned long pool_size = CONFIG_FLUX_ELASTIC_POOL_MB * SZ_1M;

		if (pool_size % SZ_2M || normal_mem_size < pool_size + SZ_64M ||
		    memory_end % SZ_2M)
			panic("invalid Flux elastic pool layout");
		bootmem_alloc_region(&base_addr, normal_mem_size - pool_size,
				     PAGE_SIZE, PAGE_SIZE, 0);
		flux_elastic_bootmem(base_addr, pool_size);
		memblock_add_node(base_addr, pool_size, 0, 0);
		memblock_reserve(base_addr, pool_size);
		base_addr += pool_size;
	}
#else
	bootmem_alloc_region(&base_addr, normal_mem_size, PAGE_SIZE, PAGE_SIZE,
			     0);
#endif
#endif
	if (base_addr != memory_end) {
		flux_debug("bootmem layout mismatch: 0x%lx != 0x%lx\n",
			   base_addr, memory_end);
		flux_ops_panic();
	}

	for (i = 0; i < nblocks; i++)
		mem_blocks[i] = memory_start + i * mem_block_size;

	update_max_low_pfn(memory_end);
	min_low_pfn = PFN_DOWN(memory_start);

	flux_debug("kernel memory: 0x%lx - 0x%lx\n", memory_start, memory_end);
	flux_debug("kernel dma memory: 0x%lx - 0x%lx\n", memory_start,
		   memory_start + dma_mem_size);

	empty_zero_page = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	memset(empty_zero_page, 0, PAGE_SIZE);

	max_pfn = max_low_pfn;
	high_memory = (void *)(max_low_pfn << PAGE_SHIFT);
	set_max_mapnr(max_low_pfn - ARCH_PFN_OFFSET);
}

static void __init zone_sizes_init(void)
{
	unsigned long zones_max_pfn[MAX_NR_ZONES] = { 0 };

#ifdef CONFIG_ZONE_DMA
	zones_max_pfn[ZONE_DMA] = PFN_DOWN(memory_start + dma_mem_size);
#endif
	zones_max_pfn[ZONE_NORMAL] = max_low_pfn;

	free_area_init(zones_max_pfn);
}

void __init misc_mem_init(void)
{
	early_memtest(min_low_pfn << PAGE_SHIFT, max_low_pfn << PAGE_SHIFT);
	arch_numa_init();
	sparse_init();
	zone_sizes_init();
	memblock_dump_all();
}

void __init mem_init(void)
{
	memblock_free_all();
}

/*
 * In our case __init memory is not part of the page allocator so there is
 * nothing to free.
 */
void free_initmem(void)
{
}

/*
 * Memory hotplug specific functions
 */
#ifdef CONFIG_MEMORY_HOTPLUG
/*
  * After memory hotplug the variables max_pfn, max_low_pfn and high_memory need
  * updating.
  */
static void update_end_of_memory_vars(u64 start, u64 size)
{
	unsigned long end_pfn = PFN_UP(start + size);

	if (end_pfn > max_pfn) {
		max_pfn = end_pfn;
		max_low_pfn = end_pfn;
		high_memory = (void *)__va(max_pfn * PAGE_SIZE - 1) + 1;
	}
}

int arch_add_memory(int nid, u64 start, u64 size, struct mhp_params *params)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;
	int ret;

	if (!pgprot_val(params->pgprot))
		pr_err("add_memory: pgprot is not set\n");

	ret = __add_pages(nid, start_pfn, nr_pages, params);
	WARN_ON_ONCE(ret);

	/* update max_pfn, max_low_pfn and high_memory */
	update_end_of_memory_vars(start_pfn << PAGE_SHIFT,
				  nr_pages << PAGE_SHIFT);

	return ret;
}

void __ref arch_remove_memory(u64 start, u64 size, struct vmem_altmap *altmap)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;

	__remove_pages(start_pfn, nr_pages, altmap);
}

#endif /* CONFIG_MEMORY_HOTPLUG */
