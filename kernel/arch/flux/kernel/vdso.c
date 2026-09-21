// SPDX-License-Identifier: GPL-2.0
#include <linux/elf.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/time_namespace.h>
#include <asm/host_dev.h>
#include <asm/host_ops.h>
#include <asm/vdso.h>
#include <vdso/datapage.h>

extern const char flux_vdso_start[], flux_vdso_end[];
struct vdso_data *flux_vdso_data __read_mostly;
static void *vdso_code;
static unsigned long vdso_size;

static int vdso_mremap(const struct vm_special_mapping *sm,
		      struct vm_area_struct *vma)
{
	vma->vm_mm->context.vdso = vma->vm_start;
	return 0;
}

static struct vm_special_mapping vdso_mapping = {
	.name = "[vdso]",
	.mremap = vdso_mremap,
};

static vm_fault_t vvar_fault(const struct vm_special_mapping *sm,
			    struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct page *timens_page = find_timens_vvar_page(vma);
	struct page *page = virt_to_page(flux_vdso_data);

	switch (vmf->pgoff) {
	case 0:
		if (timens_page)
			page = timens_page;
		break;
	case 1:
		if (timens_page)
			break;
		fallthrough;
	default:
		return VM_FAULT_SIGBUS;
	}
	return vmf_insert_pfn(vma, vmf->address, page_to_pfn(page));
}

static const struct vm_special_mapping vvar_mapping = {
	.name = "[vvar]",
	.fault = vvar_fault,
};

/* Kernel image sections are outside Flux's managed RAM. Give Linux real
 * source pages before timekeeping starts publishing into its vvar page.
 */
void __init flux_vdso_init(void)
{
	BUILD_BUG_ON(CS_BASES * sizeof(struct vdso_data) > PAGE_SIZE);
	vdso_size = flux_vdso_end - flux_vdso_start;
	if (!vdso_size || !PAGE_ALIGNED(vdso_size) ||
	    memcmp(flux_vdso_start, ELFMAG, SELFMAG))
		panic("invalid built-in Flux vDSO");

	flux_vdso_data = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	vdso_code = memblock_alloc(vdso_size, PAGE_SIZE);
	if (!flux_vdso_data || !vdso_code)
		panic("cannot allocate Flux vDSO pages");
	memcpy(vdso_code, flux_vdso_start, vdso_size);
	/* Before application tasks exist, validate the immutable shared image
	 * through the same instruction backend used by executable mappings.
	 */
	if (flux_ops_rewrite_exec(vdso_code, vdso_size))
		panic("invalid executable bytes in Flux vDSO");
}

static int __init vdso_init_pages(void)
{
	unsigned int i, count = vdso_size >> PAGE_SHIFT;
	struct page **pages = kcalloc(count + 1, sizeof(*pages), GFP_KERNEL);

	if (!pages)
		panic("cannot allocate Flux vDSO page list");
	for (i = 0; i < count; i++)
		pages[i] = virt_to_page(vdso_code + i * PAGE_SIZE);
	vdso_mapping.pages = pages;
	return 0;
}
arch_initcall(vdso_init_pages);

#ifdef CONFIG_TIME_NS
struct vdso_data *arch_get_vdso_data(void *vvar_page)
{
	return vvar_page;
}

int vdso_join_timens(struct task_struct *task, struct time_namespace *ns)
{
	struct mm_struct *mm = task->mm;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	mmap_read_lock(mm);
	for_each_vma(vmi, vma)
		if (vma_is_special_mapping(vma, &vvar_mapping))
			zap_vma_pages(vma);
	mmap_read_unlock(mm);
	return 0;
}
#endif

int flux_map_vdso(struct mm_struct *mm)
{
	unsigned long base, vvar_size = FLUX_VVAR_PAGES * PAGE_SIZE;
	struct vm_area_struct *vma;
	int ret;

	if (mmap_write_lock_killable(mm))
		return -EINTR;
	mm->context.vdso = 0;
	base = get_unmapped_area(NULL, 0, vvar_size + vdso_size, 0, 0);
	ret = (long)base;
	if (IS_ERR_VALUE(base))
		goto out;
	ret = flux_host_dev_reserve_alias_range(base, vvar_size + vdso_size, false);
	if (ret)
		goto out;
	vma = _install_special_mapping(mm, base, vvar_size,
		VM_READ | VM_MAYREAD | VM_IO | VM_PFNMAP | VM_DONTDUMP,
		&vvar_mapping);
	ret = PTR_ERR_OR_ZERO(vma);
	if (ret)
		goto out;
	base += vvar_size;
	vma = _install_special_mapping(mm, base, vdso_size,
		VM_READ | VM_EXEC | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC,
		&vdso_mapping);
	ret = PTR_ERR_OR_ZERO(vma);
	if (!ret)
		mm->context.vdso = base;
out:
	mmap_write_unlock(mm);
	return ret;
}
