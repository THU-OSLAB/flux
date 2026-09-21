/* VMA snapshots and map/smaps ioctl handlers. */

#define pr_fmt(fmt) "flux_mm: " fmt

#include "internal.h"

int (*flux_walk_page_vma)(struct vm_area_struct *,
			 const struct mm_walk_ops *, void *);
int (*flux_smaps_pte_range)(pmd_t *, unsigned long, unsigned long,
			 struct mm_walk *);
struct mm_walk_ops flux_smaps_walk_ops = {
	.walk_lock = PGWALK_RDLOCK,
};

void flux_mm_ranges_clear(struct list_head *head)
{
	struct flux_mm_range *range, *tmp;

	list_for_each_entry_safe(range, tmp, head, list) {
		list_del(&range->list);
		if (range->file)
			fput(range->file);
		kfree(range);
	}
}

struct flux_mm_range *flux_mm_range_from_vma(struct vm_area_struct *vma,
						 gfp_t gfp)
{
	struct flux_mm_range *range;

	range = kmalloc(sizeof(*range), gfp);
	if (!range)
		return NULL;

	range->start = vma->vm_start;
	range->end = vma->vm_end;
	range->pgoff = vma->vm_pgoff;
	range->flags = vma->vm_flags;
	range->file = vma->vm_file;
	if (range->file)
		get_file(range->file);

	return range;
}

bool flux_mm_range_list_overlaps(struct list_head *head,
					unsigned long start, unsigned long end)
{
	struct flux_mm_range *range;

	list_for_each_entry(range, head, list) {
		if (start < range->end && end > range->start)
			return true;
	}

	return false;
}

static int flux_mm_ranges_snapshot(struct mm_struct *mm, struct list_head *head)
{
	struct vma_iterator vmi;
	struct vm_area_struct *vma;
	struct flux_mm_range *range;
	int err = 0;

	if (mmap_read_lock_killable(mm))
		return -EINTR;

	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma) {
		range = flux_mm_range_from_vma(vma, GFP_KERNEL);
		if (!range) {
			err = -ENOMEM;
			break;
		}
		list_add_tail(&range->list, head);
	}

	if (err)
		flux_mm_ranges_clear(head);

	mmap_read_unlock(mm);

	return err;
}

int flux_mm_record_base_maps(struct flux_mm_ctx *ctx,
					 struct mm_struct *mm)
{
	LIST_HEAD(ranges);
	struct mm_struct *exec_mm;
	int err;

	err = flux_mm_ranges_snapshot(mm, &ranges);
	if (err)
		return err;

	/*
	 * END_MAP_SHARED is the runtime/application boundary. Capture execution
	 * infrastructure once, before ELF loading can add application mappings.
	 */
	mutex_lock(&ctx->lock);
	if (ctx->exec_mm) {
		err = -EBUSY;
		goto out;
	}
	/* VM_DONTCOPY keeps projections out of the execution template. */
	exec_mm = flux_dup_mm(current, mm);
	err = exec_mm ? 0 : -ENOMEM;
	if (err)
		goto out;
	ctx->exec_mm = exec_mm;
	flux_mm_ranges_clear(&ctx->base_maps);
	list_splice_tail_init(&ranges, &ctx->base_maps);
out:
	mutex_unlock(&ctx->lock);
	flux_mm_ranges_clear(&ranges);
	return err;
}

int flux_mm_get_base_maps(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_mpk_base_maps req;
	struct flux_mpk_base_map *maps = NULL;
	struct flux_mm_range *range;
	unsigned int nr = 0, i = 0;
	int err = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	list_for_each_entry(range, &ctx->base_maps, list)
		nr++;

	req.nr = nr;
	if (!nr) {
		err = -ENOENT;
		goto out_copy_req;
	}
	if (req.capacity < nr) {
		err = -ENOSPC;
		goto out_copy_req;
	}
	if (!req.maps) {
		err = -EINVAL;
		goto out_copy_req;
	}

	maps = kcalloc(nr, sizeof(*maps), GFP_KERNEL);
	if (!maps) {
		err = -ENOMEM;
		goto out_copy_req;
	}

	list_for_each_entry(range, &ctx->base_maps, list) {
		maps[i].start = range->start;
		maps[i].end = range->end;
		i++;
	}

out_copy_req:
	mutex_unlock(&ctx->lock);
	if (!err && copy_to_user((void __user *)req.maps, maps,
				 nr * sizeof(*maps)))
		err = -EFAULT;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		err = -EFAULT;
	kfree(maps);
	return err;
}

int flux_mm_get_app_maps(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_mm_app_maps req;
	struct flux_mpk_base_map *maps = NULL;
	struct vm_area_struct *vma;
	struct flux_proc *proc;
	struct mm_struct *mm;
	struct vma_iterator vmi;
	unsigned int nr = 0;
	int err = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.proc_key < 0)
		return -EINVAL;
	if (req.capacity && !req.maps)
		return -EINVAL;
	if (req.capacity > 65536)
		return -E2BIG;

	proc = flux_proc_get_by_key(ctx, req.proc_key);
	if (!proc)
		return -ENOENT;

	mm = proc->mm;
	if (!mm) {
		err = -ENOENT;
		goto out_put;
	}
	if (current->mm != mm) {
		err = -ESTALE;
		goto out_put;
	}

	if (req.capacity) {
		maps = kvcalloc(req.capacity, sizeof(*maps), GFP_KERNEL);
		if (!maps) {
			err = -ENOMEM;
			goto out_put;
		}
	}

	if (mmap_read_lock_killable(mm)) {
		err = -EINTR;
		goto out_free;
	}
	mutex_lock(&ctx->lock);
	if (!READ_ONCE(ctx->mpk_enabled) && list_empty(&ctx->base_maps)) {
		err = -ENOENT;
		goto out_unlock;
	}

	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma) {
		bool app_vma;

		if (READ_ONCE(ctx->mpk_enabled))
			app_vma = vma_pkey(vma) == FLUX_MPK_APP_PKEY;
		else
			app_vma = !flux_mm_range_list_overlaps(
				&ctx->base_maps, vma->vm_start, vma->vm_end);
		if (!app_vma)
			continue;

		if (nr < req.capacity) {
			maps[nr].start = vma->vm_start;
			maps[nr].end = vma->vm_end;
		}
		nr++;
	}
	req.nr = nr;
	if (req.capacity < nr)
		err = -ENOSPC;

out_unlock:
	mutex_unlock(&ctx->lock);
	mmap_read_unlock(mm);
	if (!err && nr && copy_to_user((void __user *)req.maps, maps,
				       nr * sizeof(*maps)))
		err = -EFAULT;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		err = -EFAULT;
out_free:
	kvfree(maps);
out_put:
	flux_proc_put(ctx, req.proc_key, proc);
	return err;
}

int flux_mm_get_app_smaps(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_host_mem_size_stats mss = {};
	struct flux_mm_smaps_stats req;
	struct vm_area_struct *vma;
	struct flux_proc *proc;
	struct mm_struct *mm;
	struct vma_iterator vmi;
	int proc_key;
	int err = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	proc_key = req.proc_key;
	if (proc_key < 0)
		return -EINVAL;
	if (!READ_ONCE(ctx->mpk_enabled) || !flux_walk_page_vma ||
	    !flux_smaps_walk_ops.pmd_entry)
		return -EOPNOTSUPP;

	proc = flux_proc_get_by_key(ctx, proc_key);
	if (!proc)
		return -ENOENT;

	mm = proc->mm;
	if (!mm) {
		err = -ENOENT;
		goto out_put;
	}
	if (current->mm != mm) {
		err = -ESTALE;
		goto out_put;
	}

	memset(&req, 0, sizeof(req));
	req.proc_key = proc_key;
	if (mmap_read_lock_killable(mm)) {
		err = -EINTR;
		goto out_put;
	}

	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma) {
		if (vma_pkey(vma) != FLUX_MPK_APP_PKEY)
			continue;
		if (is_vm_hugetlb_page(vma)) {
			err = -EOPNOTSUPP;
			goto out_unlock_smaps;
		}

		if (!req.nr_vmas)
			req.start = vma->vm_start;
		req.end = vma->vm_end;
		req.size += vma->vm_end - vma->vm_start;
		req.nr_vmas++;

		err = flux_walk_page_vma(vma, &flux_smaps_walk_ops, &mss);
		if (err)
			goto out_unlock_smaps;
	}
	if (!req.nr_vmas) {
		err = -ENOENT;
		goto out_unlock_smaps;
	}

	req.resident = mss.resident;
	req.pss = mss.pss >> FLUX_SMAPS_PSS_SHIFT;
	req.pss_dirty = mss.pss_dirty >> FLUX_SMAPS_PSS_SHIFT;
	req.pss_anon = mss.pss_anon >> FLUX_SMAPS_PSS_SHIFT;
	req.pss_file = mss.pss_file >> FLUX_SMAPS_PSS_SHIFT;
	req.pss_shmem = mss.pss_shmem >> FLUX_SMAPS_PSS_SHIFT;
	req.shared_clean = mss.shared_clean;
	req.shared_dirty = mss.shared_dirty;
	req.private_clean = mss.private_clean;
	req.private_dirty = mss.private_dirty;
	req.referenced = mss.referenced;
	req.anonymous = mss.anonymous;
	req.ksm = mss.ksm;
	req.lazyfree = mss.lazyfree;
	req.anonymous_thp = mss.anonymous_thp;
	req.shmem_thp = mss.shmem_thp;
	req.file_thp = mss.file_thp;
	req.shared_hugetlb = mss.shared_hugetlb;
	req.private_hugetlb = mss.private_hugetlb;
	req.swap = mss.swap;
	req.swap_pss = mss.swap_pss >> FLUX_SMAPS_PSS_SHIFT;
	req.locked = mss.pss_locked >> FLUX_SMAPS_PSS_SHIFT;

out_unlock_smaps:
	mmap_read_unlock(mm);
	if (!err && copy_to_user((void __user *)arg, &req, sizeof(req)))
		err = -EFAULT;
out_put:
	flux_proc_put(ctx, proc_key, proc);
	return err;
}
