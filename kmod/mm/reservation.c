// SPDX-License-Identifier: GPL-2.0
/* Host reservation mechanism; alias source and PTE policy live elsewhere. */

#include <linux/anon_inodes.h>
#include <linux/cred.h>
#include <linux/mman.h>

#include "internal.h"

/*
 * A stateless, independent file identifies a prepared reservation. It owns no
 * ctx/mm/source references, so VMA and snapshot file refs cannot form a cycle.
 * Host fork omits projections; the child rebuilds them from its LibOS PTEs.
 * In particular, no copied host PTE can outlive a source pin in the parent.
 */
static int flux_alias_reservation_mmap(struct file *file,
				       struct vm_area_struct *vma)
{
	if (vma->vm_pgoff ||
	    (vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYWRITE | VM_WRITE);
	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	return 0;
}

static const struct file_operations flux_alias_reservation_ops = {
	.owner = THIS_MODULE,
	.mmap = flux_alias_reservation_mmap,
};

/* Identity, not PFNMAP alone, distinguishes our stateless reservations. */
bool flux_alias_reservation_vma(const struct vm_area_struct *vma)
{
	const vm_flags_t required = VM_PFNMAP | VM_DONTEXPAND |
				    VM_DONTDUMP | VM_DONTCOPY;

	return vma->vm_file &&
	       vma->vm_file->f_op == &flux_alias_reservation_ops &&
	       (vma->vm_flags & required) == required &&
	       !(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) &&
	       !vma->anon_vma && !vma->vm_private_data;
}

/*
 * Low Flux addresses can be below vm.mmap_min_addr. The alias ioctl is
 * already a trusted boundary, so retry only this reservation with a temporary
 * CAP_SYS_RAWIO credential instead of changing the host-wide policy.
 */
unsigned long flux_alias_reserve_range(unsigned long uva, unsigned long len,
				       bool shared, bool replace)
{
	const struct cred *old;
	struct cred *override;
	struct file *file;
	unsigned long reserve;
	unsigned long flags = (replace ? MAP_FIXED : MAP_FIXED_NOREPLACE) |
		(shared ? MAP_SHARED : MAP_PRIVATE);

	file = anon_inode_getfile("flux-alias-reservation",
				  &flux_alias_reservation_ops, NULL, O_RDWR);
	if (IS_ERR(file))
		return PTR_ERR(file);
	reserve = vm_mmap(file, uva, len, PROT_NONE,
			  flags, 0);
	if (reserve != (unsigned long)-EPERM)
		goto out_file;

	override = prepare_creds();
	if (!override) {
		reserve = -ENOMEM;
		goto out_file;
	}
	cap_raise(override->cap_permitted, CAP_SYS_RAWIO);
	cap_raise(override->cap_effective, CAP_SYS_RAWIO);
	old = override_creds(override);
	reserve = vm_mmap(file, uva, len, PROT_NONE,
			  flags, 0);
	revert_creds(old);
	abort_creds(override);

out_file:
	fput(file);
	return reserve;
}

/* Normal-context range preparation only; not a signal/NOWAIT operation. */
int flux_prepare_alias_reservation(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_alias_reserve_args a;
	struct vm_area_struct *vma;
	struct file *file;
	unsigned long len, flags, addr;

	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	if (!a.len || a.len > ULONG_MAX - (PAGE_SIZE - 1) ||
	    (a.user_va & ~PAGE_MASK) || a.pad ||
	    (a.flags & ~FLUX_ALIAS_F_SHARED))
		return -EINVAL;
	len = PAGE_ALIGN(a.len);
	if (a.user_va > ULONG_MAX - len)
		return -EINVAL;
	if (a.user_va < FLUX_VMALLOC_END &&
	    a.user_va + len > FLUX_VMALLOC_START)
		return -EPERM;
	file = anon_inode_getfile("flux-alias-reservation",
				  &flux_alias_reservation_ops, NULL, O_RDWR);
	if (IS_ERR(file))
		return PTR_ERR(file);
	flags = MAP_FIXED |
		(a.flags & FLUX_ALIAS_F_SHARED ? MAP_SHARED : MAP_PRIVATE);
	mutex_lock(&ctx->lock);
	if (flux_mm_range_list_overlaps(&ctx->base_maps, a.user_va,
					a.user_va + len)) {
		mutex_unlock(&ctx->lock);
		fput(file);
		return -EPERM;
	}
	mutex_lock(flux_alias_mm_lock(current->mm));
	/*
	 * Linux application mappings replace only our own reservations. Runtime
	 * allocations made after the base snapshot still belong to the runtime.
	 */
	mmap_read_lock(current->mm);
	vma = find_vma_intersection(current->mm, a.user_va, a.user_va + len);
	while (vma && vma->vm_start < a.user_va + len) {
		if (!vma->vm_file ||
		    vma->vm_file->f_op != &flux_alias_reservation_ops) {
			mmap_read_unlock(current->mm);
			addr = -EPERM;
			goto out_unlock;
		}
		vma = find_vma(current->mm, vma->vm_end);
	}
	mmap_read_unlock(current->mm);
	/* vm_mmap retains host address/limit and VMA accounting checks. */
	addr = vm_mmap(file, a.user_va, len, PROT_NONE, flags, 0);
	if (!IS_ERR_VALUE(addr) && addr != a.user_va) {
		vm_munmap(addr, len);
		addr = -EFAULT;
	}
out_unlock:
	mutex_unlock(flux_alias_mm_lock(current->mm));
	mutex_unlock(&ctx->lock);
	fput(file);
	/* ioctl dispatch narrows its result to int: success is status, not a VA. */
	return IS_ERR_VALUE(addr) ? (long)addr : 0;
}
