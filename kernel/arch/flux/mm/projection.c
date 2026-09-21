// SPDX-License-Identifier: GPL-2.0
/* Fork construction publishes VMA admission; the host resolves leaves lazily. */
#include <linux/mm.h>
#include <linux/slab.h>

#include <asm/host_dev.h>
#include <asm/mm_hooks.h>
#include <asm/page.h>
#include <asm/pgtable.h>

static_assert(_PAGE_PRESENT == FLUX_PROJECTION_PT_PRESENT);
static_assert(_PAGE_PSE == FLUX_PROJECTION_PT_HUGE);
static_assert(_PAGE_PROTNONE == FLUX_PROJECTION_PT_NONE);
static_assert(_PAGE_USER == FLUX_PROJECTION_PT_USER);

/* Registration scales with VMAs, not resident pages or parent host aliases. */
void flux_prepare_fork_projection(struct mm_struct *mm)
{
	struct flux_bind_fork_projection a = {
		.proc_key = mm->context.proc_key,
		.pgd = (unsigned long)mm->pgd,
		.memory_start = memory_start,
		.memory_end = memory_end,
	};
	struct flux_projection_range *ranges;
	struct vm_area_struct *vma;
	unsigned int capacity =
		min_t(unsigned int, mm->map_count, FLUX_PROJECTION_MAX_RANGES);
	VMA_ITERATOR(vmi, mm, 0);

	mmap_assert_write_locked(mm);
	if (!capacity)
		return;
	ranges = kmalloc_array(capacity, sizeof(*ranges), GFP_KERNEL);
	if (!ranges)
		return;
	for_each_vma(vmi, vma)
	{
		if (!vma_is_anonymous(vma) || !(vma->vm_flags & VM_READ) ||
		    (vma->vm_flags & (VM_SHARED | VM_EXEC | VM_HUGETLB |
				      VM_PFNMAP | VM_MIXEDMAP | VM_IO)))
			continue;
		if (a.nr_ranges == capacity)
			break;
		ranges[a.nr_ranges++] = (struct flux_projection_range){
			.start = vma->vm_start,
			.end = vma->vm_end,
		};
	}
	if (a.nr_ranges) {
		a.ranges = (unsigned long)ranges;
		(void)flux_host_dev_call_mm(FLUX_DEV_IO_BIND_FORK_PROJECTION,
					    (unsigned long)&a);
	}
	kfree(ranges);
}
