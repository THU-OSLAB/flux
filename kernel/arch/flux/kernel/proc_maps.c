// SPDX-License-Identifier: GPL-2.0

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/ioctl.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <uapi/asm/mpk.h>
#include <uapi/asm/rewrite.h>
#include <linux/uaccess.h>

#include <asm/proc_maps.h>
#include <asm/syscalls.h>

#define FLUX_PROCFS_IOCTL_MAGIC 'f'

enum flux_procmap_query_flags {
	FLUX_PROCMAP_QUERY_VMA_READABLE		= 0x01,
	FLUX_PROCMAP_QUERY_VMA_WRITABLE		= 0x02,
	FLUX_PROCMAP_QUERY_VMA_EXECUTABLE	= 0x04,
	FLUX_PROCMAP_QUERY_VMA_SHARED		= 0x08,
	FLUX_PROCMAP_QUERY_COVERING_OR_NEXT_VMA	= 0x10,
	FLUX_PROCMAP_QUERY_FILE_BACKED_VMA	= 0x20,
};

struct flux_procmap_query {
	u64 size;
	u64 query_flags;
	u64 query_addr;
	u64 vma_start;
	u64 vma_end;
	u64 vma_flags;
	u64 vma_page_size;
	u64 vma_offset;
	u64 inode;
	u32 dev_major;
	u32 dev_minor;
	u32 vma_name_size;
	u32 build_id_size;
	u64 vma_name_addr;
	u64 build_id_addr;
};

#define FLUX_PROCMAP_QUERY \
	_IOWR(FLUX_PROCFS_IOCTL_MAGIC, 17, struct flux_procmap_query)
#define FLUX_PROCMAP_QUERY_PERM_MASK \
	(FLUX_PROCMAP_QUERY_VMA_READABLE | FLUX_PROCMAP_QUERY_VMA_WRITABLE | \
	 FLUX_PROCMAP_QUERY_VMA_EXECUTABLE | FLUX_PROCMAP_QUERY_VMA_SHARED)
#define FLUX_PROCMAP_QUERY_KNOWN_MASK \
	(FLUX_PROCMAP_QUERY_PERM_MASK | \
	 FLUX_PROCMAP_QUERY_COVERING_OR_NEXT_VMA | \
	 FLUX_PROCMAP_QUERY_FILE_BACKED_VMA)

/*
 * These regions are allocated by the host runtime before Linux exec. They
 * have no pages in Flux's managed RAM: host execution inherits their backing
 * directly, and Linux must not populate a replacement page on a fault.
 */
static vm_fault_t flux_runtime_fault(const struct vm_special_mapping *sm,
				    struct vm_area_struct *vma,
				    struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static const struct vm_special_mapping flux_runtime_mapping = {
	.name = "[flux-runtime]",
	.fault = flux_runtime_fault,
};

int flux_map_runtime(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	unsigned long flags = VM_READ | VM_MAYREAD | VM_SHARED | VM_MAYSHARE |
		VM_IO | VM_PFNMAP | VM_DONTDUMP;
	int ret;

	if (mmap_write_lock_killable(mm))
		return -EINTR;
	vma = _install_special_mapping(mm, FLUX_RODATA_ADDR,
		FLUX_RODATA_HEADER_SIZE, flags, &flux_runtime_mapping);
	ret = PTR_ERR_OR_ZERO(vma);
	if (ret)
		goto out;
	vma = _install_special_mapping(mm, FLUX_MPK_RETURN_ADDR,
		FLUX_MPK_RETURN_AREA_SIZE, flags | VM_WRITE | VM_MAYWRITE,
		&flux_runtime_mapping);
	ret = PTR_ERR_OR_ZERO(vma);
out:
	mmap_write_unlock(mm);
	return ret;
}

long arch_proc_maps_ioctl(struct mm_struct *mm, unsigned int cmd,
			  unsigned long arg)
{
	struct flux_procmap_query q;
	struct vma_iterator vmi;
	struct vm_area_struct *vma = NULL;
	struct inode *inode;
	u64 vma_flags;
	u64 perm_filter;
	size_t copy_size;
	dev_t dev;
	int ret;

	if (cmd != FLUX_PROCMAP_QUERY)
		return -ENOTTY;

	if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
		return -EFAULT;
	if (q.size < sizeof(q))
		return -EINVAL;
	if (q.query_flags & ~FLUX_PROCMAP_QUERY_KNOWN_MASK)
		return -EINVAL;
	if (q.build_id_size || q.build_id_addr)
		return -EOPNOTSUPP;
	if (!!q.vma_name_addr != !!q.vma_name_size)
		return -EINVAL;
	if (!mm)
		return -ESRCH;

	mmap_read_lock(mm);
	vma_iter_init(&vmi, mm, (unsigned long)q.query_addr);
	for_each_vma(vmi, vma) {
		if (q.query_addr < vma->vm_start &&
		    !(q.query_flags &
		      FLUX_PROCMAP_QUERY_COVERING_OR_NEXT_VMA)) {
			vma = NULL;
			break;
		}

		if ((q.query_flags & FLUX_PROCMAP_QUERY_FILE_BACKED_VMA) &&
		    !vma->vm_file)
			goto next_vma;

		vma_flags = 0;
		if (vma->vm_flags & VM_READ)
			vma_flags |= FLUX_PROCMAP_QUERY_VMA_READABLE;
		if (vma->vm_flags & VM_WRITE)
			vma_flags |= FLUX_PROCMAP_QUERY_VMA_WRITABLE;
		if (vma->vm_flags & VM_EXEC)
			vma_flags |= FLUX_PROCMAP_QUERY_VMA_EXECUTABLE;
		if (vma->vm_flags & VM_MAYSHARE)
			vma_flags |= FLUX_PROCMAP_QUERY_VMA_SHARED;

		perm_filter = q.query_flags & FLUX_PROCMAP_QUERY_PERM_MASK;
		if ((vma_flags & perm_filter) != perm_filter)
			goto next_vma;
		break;

next_vma:
		if (!(q.query_flags &
		      FLUX_PROCMAP_QUERY_COVERING_OR_NEXT_VMA)) {
			vma = NULL;
			break;
		}
	}

	if (!vma) {
		ret = -ENOENT;
		goto out_unlock;
	}

	q.vma_start = vma->vm_start;
	q.vma_end = vma->vm_end;
	q.vma_flags = vma_flags;
	q.vma_page_size = vma_kernel_pagesize(vma);
	/* Match /proc/$pid/maps: anonymous VMAs always report offset zero. */
	q.vma_offset = vma->vm_file ?
		(loff_t)vma->vm_pgoff << PAGE_SHIFT : 0;
	q.inode = 0;
	q.dev_major = 0;
	q.dev_minor = 0;
	q.build_id_size = 0;

	if (vma->vm_file) {
		inode = file_inode(vma->vm_file);
		dev = inode->i_sb->s_dev;
		q.inode = inode->i_ino;
		q.dev_major = MAJOR(dev);
		q.dev_minor = MINOR(dev);
	}

	if (q.vma_name_size) {
		char *buf = kmalloc(PATH_MAX, GFP_KERNEL);
		const char *name = vma->vm_ops && vma->vm_ops->name ?
			vma->vm_ops->name(vma) : NULL;
		u32 len = 0;

		if (!buf) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		if (name) {
			strscpy(buf, name, PATH_MAX);
			len = strlen(buf) + 1;
		} else if (vma->vm_file) {
			char *pathname = file_path(vma->vm_file, buf, PATH_MAX);

			if (IS_ERR(pathname)) {
				ret = PTR_ERR(pathname);
				kfree(buf);
				goto out_unlock;
			}
			len = strlen(pathname) + 1;
			memmove(buf, pathname, len);
		} else if (vma_is_initial_heap(vma)) {
			strscpy(buf, "[heap]", PATH_MAX);
			len = strlen(buf) + 1;
		} else if (vma_is_initial_stack(vma)) {
			strscpy(buf, "[stack]", PATH_MAX);
			len = strlen(buf) + 1;
		}

		if (len > q.vma_name_size) {
			ret = -E2BIG;
			kfree(buf);
			goto out_unlock;
		}
		if (len && copy_to_user((void __user *)(unsigned long)
					q.vma_name_addr, buf, len)) {
			ret = -EFAULT;
			kfree(buf);
			goto out_unlock;
		}
		kfree(buf);
		q.vma_name_size = len;
	} else {
		q.vma_name_size = 0;
	}

	copy_size = min_t(size_t, q.size, sizeof(q));
	if (copy_to_user((void __user *)arg, &q, copy_size))
		ret = -EFAULT;
	else
		ret = 0;

out_unlock:
	mmap_read_unlock(mm);
	return ret;
}
