// SPDX-License-Identifier: GPL-2.0
/* Explicit, revocable backing for the optional Flux extent allocator. */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/page_ref.h>
#include <linux/pagemap.h>
#include <linux/swap.h>

#include <asm/pgtable.h>

#include <flux/host_abi.h>

#include "internal.h"

#define EXTENT_PAGES (FLUX_ELASTIC_EXTENT_SIZE / PAGE_SIZE)
#define ELASTIC_FAULT_BATCH_MAX 64U

static unsigned int elastic_fault_batch = 1;
module_param(elastic_fault_batch, uint, 0444);
MODULE_PARM_DESC(elastic_fault_batch,
		 "Maximum adaptive backing fault-around pages (1-64)");

struct flux_backing_extent {
	/* fault installation and backing replacement share this lock. */
	struct mutex lock;
	struct page **pages;
	u64 generation;
	bool available;
	/* Placement hint only: never dereferenced and never grants access. */
	struct mm_struct *last_fault_mm;
	unsigned long next_fault;
	unsigned int fault_window;
};

struct flux_backing_domain {
	unsigned int nr_extents;
	struct flux_backing_extent *extents;
	atomic64_t resident_pages;
	atomic64_t commits;
	atomic64_t decommits;
	atomic64_t busy;
	atomic64_t faults;
	atomic64_t denied_faults;
};

static atomic_long_t elastic_resident_pages = ATOMIC_LONG_INIT(0);

static int elastic_pages_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%ld\n",
			 atomic_long_read(&elastic_resident_pages));
}

static const struct kernel_param_ops elastic_pages_ops = {
	.get = elastic_pages_get,
};
module_param_cb(elastic_resident_pages, &elastic_pages_ops, NULL, 0444);
MODULE_PARM_DESC(elastic_resident_pages, "Unique pages owned by elastic domains");

static void flux_backing_free_pages(struct flux_backing_extent *extent)
{
	unsigned int i, nr = 0;

	if (!extent->pages)
		return;
	for (i = 0; i < EXTENT_PAGES; i++) {
		if (extent->pages[i])
			extent->pages[nr++] = extent->pages[i];
	}
	release_pages(extent->pages, nr);
	kfree(extent->pages);
	extent->pages = NULL;
}

static int flux_backing_commit(struct flux_backing_domain *domain,
			       struct flux_backing_extent *extent)
{
	unsigned int populated = 0, next;

	if (extent->available)
		return -EALREADY;
	extent->pages = kcalloc(EXTENT_PAGES, sizeof(*extent->pages), GFP_KERNEL);
	if (!extent->pages)
		return -ENOMEM;
	/* The bulk helper may make partial progress; keep already-filled slots. */
	while (populated < EXTENT_PAGES) {
		next = flux_compat_alloc_pages_bulk(GFP_KERNEL | __GFP_ZERO,
						    EXTENT_PAGES, extent->pages);
		if (next <= populated) {
			flux_backing_free_pages(extent);
			return -ENOMEM;
		}
		populated = next;
	}
	/* Pages cannot be faulted in until the whole extent is ready. */
	extent->available = true;
	extent->last_fault_mm = NULL;
	extent->fault_window = 0;
	extent->generation++;
	atomic64_add(EXTENT_PAGES, &domain->resident_pages);
	atomic_long_add(EXTENT_PAGES, &elastic_resident_pages);
	atomic64_inc(&domain->commits);
	return 0;
}

static int flux_backing_decommit(struct file *file, unsigned int index)
{
	struct flux_backing_domain *domain = file->private_data;
	struct flux_backing_extent *extent = &domain->extents[index];
	unsigned int i, frozen = 0;
	int ret = 0;

	if (!extent->available)
		return -EALREADY;
	/*
	 * The caller holds extent->lock. New fault installations must wait.
	 * First remove *all* file mappings, including inactive Flux mms and
	 * copies inherited by native fork. No user VMA/PTE may reacquire a page
	 * while we test references. The private inode makes this domain-local.
	 */
	extent->available = false;
	extent->last_fault_mm = NULL;
	extent->fault_window = 0;
	unmap_mapping_range(file->f_mapping,
			    (loff_t)index << FLUX_ELASTIC_EXTENT_SHIFT,
			    FLUX_ELASTIC_EXTENT_SIZE, 1);
	/*
	 * A raw alias, GUP pin, or any other external page reference makes the
	 * extent busy. Freeze the sole ownership reference atomically instead
	 * of racing a page_count() snapshot with an in-flight fast GUP.
	 */
	for (i = 0; i < EXTENT_PAGES; i++) {
		if (!page_ref_freeze(extent->pages[i], 1)) {
			ret = -EBUSY;
			break;
		}
		frozen++;
	}
	for (i = 0; i < frozen; i++)
		page_ref_unfreeze(extent->pages[i], 1);
	if (ret) {
		extent->available = true;
		atomic64_inc(&domain->busy);
		return ret;
	}
	flux_backing_free_pages(extent);
	extent->generation++;
	atomic64_sub(EXTENT_PAGES, &domain->resident_pages);
	atomic_long_sub(EXTENT_PAGES, &elastic_resident_pages);
	atomic64_inc(&domain->decommits);
	return 0;
}

static vm_fault_t flux_backing_fault(struct vm_fault *vmf)
{
	struct flux_backing_domain *domain = vmf->vma->vm_file->private_data;
	unsigned long index = vmf->pgoff / EXTENT_PAGES;
	struct flux_backing_extent *extent;
	vm_fault_t ret;

	if (index >= domain->nr_extents)
		return VM_FAULT_SIGBUS;
	extent = &domain->extents[index];
	mutex_lock(&extent->lock);
	if (!extent->available) {
		/* A stray access must not silently undo a completed shrink. */
		atomic64_inc(&domain->denied_faults);
		ret = VM_FAULT_SIGBUS;
	} else {
		unsigned long offset = vmf->pgoff % EXTENT_PAGES;
		unsigned int maximum = clamp_t(unsigned int, elastic_fault_batch,
					       1, ELASTIC_FAULT_BATCH_MAX);
		unsigned long nr = 1, remaining, installed;
		int error;

		atomic64_inc(&domain->faults);
		if (maximum == 1) {
			ret = vmf_insert_page(vmf->vma, vmf->address,
					      extent->pages[offset]);
			goto out;
		}
		if (extent->last_fault_mm == vmf->vma->vm_mm &&
		    extent->next_fault == vmf->pgoff)
			nr = min(maximum, max(1U, extent->fault_window * 2));
		nr = min(nr, EXTENT_PAGES - offset);
		nr = min(nr, (vmf->vma->vm_end - vmf->address) >> PAGE_SHIFT);
		nr = min(nr, PTRS_PER_PTE - pte_index(vmf->address));
		remaining = nr;
		/* VM_MIXEDMAP is established at mmap, before fault-time insertion.
		 * The extent mutex excludes generation changes and revocation for
		 * the whole batch. Each installed page gets its normal map reference.
		 */
		error = vm_insert_pages(vmf->vma, vmf->address,
					extent->pages + offset, &remaining);
		installed = nr - remaining;
		if (installed || error == -EBUSY) {
			extent->last_fault_mm = vmf->vma->vm_mm;
			extent->fault_window = max(installed, 1UL);
			extent->next_fault = vmf->pgoff + max(installed, 1UL);
			ret = VM_FAULT_NOPAGE;
		} else {
			ret = error == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS;
		}
	}
out:
	mutex_unlock(&extent->lock);
	return ret;
}

static const struct vm_operations_struct flux_backing_vm_ops = {
	.fault = flux_backing_fault,
};

/* Revoke updates every VMA through this file's address_space. */
bool flux_backing_vma_shared(const struct vm_area_struct *vma)
{
	return vma->vm_ops == &flux_backing_vm_ops;
}

static int flux_backing_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct flux_backing_domain *domain = file->private_data;
	unsigned long nr_pages = vma_pages(vma);
	unsigned long max_pages = (unsigned long)domain->nr_extents * EXTENT_PAGES;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
	    vma->vm_pgoff >= max_pages || nr_pages > max_pages - vma->vm_pgoff)
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYEXEC);
	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &flux_backing_vm_ops;
	return 0;
}

static long flux_backing_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct flux_backing_domain *domain = file->private_data;
	struct flux_elastic_extent req;
	struct flux_backing_extent *extent;
	int ret = 0;

	if (cmd == FLUX_ELASTIC_INFO) {
		struct flux_elastic_info info = {
			.nr_extents = domain->nr_extents,
			.extent_shift = FLUX_ELASTIC_EXTENT_SHIFT,
			.resident_pages = atomic64_read(&domain->resident_pages),
			.commits = atomic64_read(&domain->commits),
			.decommits = atomic64_read(&domain->decommits),
			.busy = atomic64_read(&domain->busy),
			.faults = atomic64_read(&domain->faults),
			.denied_faults = atomic64_read(&domain->denied_faults),
		};

		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
			-EFAULT : 0;
	}
	if (cmd != FLUX_ELASTIC_QUERY && cmd != FLUX_ELASTIC_COMMIT &&
	    cmd != FLUX_ELASTIC_DECOMMIT)
		return -ENOTTY;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.reserved || req.index >= domain->nr_extents)
		return -EINVAL;
	extent = &domain->extents[req.index];
	mutex_lock(&extent->lock);
	if (cmd != FLUX_ELASTIC_QUERY) {
		if (req.generation != extent->generation)
			ret = -ESTALE;
		else if (extent->generation == U64_MAX)
			ret = -EOVERFLOW;
		else if (cmd == FLUX_ELASTIC_COMMIT)
			ret = flux_backing_commit(domain, extent);
		else
			ret = flux_backing_decommit(file, req.index);
	}
	req.generation = extent->generation;
	req.state = extent->available ? FLUX_ELASTIC_AVAILABLE : FLUX_ELASTIC_UNBACKED;
	req.resident_pages = extent->available ? EXTENT_PAGES : 0;
	mutex_unlock(&extent->lock);
	/* On copyout failure QUERY reconciles the actual committed state. */
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static int flux_backing_release(struct inode *inode, struct file *file)
{
	struct flux_backing_domain *domain = file->private_data;
	unsigned int i;

	/* Last file reference: no VMA or fault can still enter this domain. */
	for (i = 0; i < domain->nr_extents; i++) {
		if (domain->extents[i].available)
			atomic_long_sub(EXTENT_PAGES, &elastic_resident_pages);
		flux_backing_free_pages(&domain->extents[i]);
	}
	kfree(domain->extents);
	kfree(domain);
	return 0;
}

static const struct file_operations flux_backing_ops = {
	.owner = THIS_MODULE,
	.mmap = flux_backing_mmap,
	.unlocked_ioctl = flux_backing_ioctl,
	.release = flux_backing_release,
};

int flux_elastic_create(unsigned long arg)
{
	struct flux_elastic_create req;
	struct flux_backing_domain *domain;
	struct file *file;
	unsigned int i;
	int fd;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.flags || !req.nr_extents || req.nr_extents > FLUX_ELASTIC_MAX_EXTENTS)
		return -EINVAL;
	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return -ENOMEM;
	domain->nr_extents = req.nr_extents;
	domain->extents = kcalloc(req.nr_extents, sizeof(*domain->extents), GFP_KERNEL);
	if (!domain->extents) {
		kfree(domain);
		return -ENOMEM;
	}
	for (i = 0; i < req.nr_extents; i++) {
		mutex_init(&domain->extents[i].lock);
		domain->extents[i].generation = 1;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		goto out_free;
	/* A distinct inode is required for domain-local mapping invalidation. */
	file = anon_inode_create_getfile("flux-elastic", &flux_backing_ops,
					domain, O_RDWR, NULL);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		fd = PTR_ERR(file);
		goto out_free;
	}
	fd_install(fd, file);
	return fd;
out_free:
	kfree(domain->extents);
	kfree(domain);
	return fd;
}
