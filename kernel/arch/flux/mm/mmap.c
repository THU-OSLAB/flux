#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/rmap.h>
#include <asm/host_dev.h>

/* Flux physical addresses are identity-mapped inside this host-backed window. */
int valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	if (addr < memory_start || addr > memory_end)
		return 0;

	return size <= memory_end - addr;
}

int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	if (pfn > memory_end >> PAGE_SHIFT)
		return 0;

	return valid_phys_addr_range((phys_addr_t)pfn << PAGE_SHIFT, size);
}

unsigned long arch_get_unmapped_area(struct file *filp, unsigned long addr,
				     unsigned long len, unsigned long pgoff,
				     unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct vm_unmapped_area_info info = { 0 };
	unsigned long begin = mm->mmap_base, end = STACK_TOP;

	if (flags & MAP_FIXED)
		return addr;

	if (len > end - begin)
		return -ENOMEM;

	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = find_vma(mm, addr);
		if (end - len >= addr &&
		    (!vma || addr + len <= vm_start_gap(vma)))
			return addr;
	}

	info.flags = 0;
	info.length = len;
	info.low_limit = begin;
	info.high_limit = end;
	info.align_mask = 0;
	info.align_offset = pgoff << PAGE_SHIFT;
	return vm_unmapped_area(&info);
}

/* Linux has selected/unmapped the interval; prepare only its host projection. */
int arch_prepare_mmap(unsigned long addr, unsigned long len, vm_flags_t flags)
{
	return flux_host_dev_reserve_alias_range(addr, len, flags & VM_SHARED);
}

void arch_complete_mmap(struct vm_area_struct *vma)
{
	/* First-touch signal handling uses the existing per-VMA fault path. */
	if (vma_is_anonymous(vma))
		(void)anon_vma_prepare(vma);
}
