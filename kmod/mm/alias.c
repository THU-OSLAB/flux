/* Linux alias PTEs for Flux mappings and the fork write-protection gate. */

#define pr_fmt(fmt) "flux_mm: " fmt

#include "alias-internal.h"
#include "fork_stats.h"

/*
 * Alias VMAs use SPECIAL PFNMAP PTEs that reference the Flux page
 * direct-map PFN. The mapping is not a normal host rmap. Context-wide source
 * objects hold long-term pins; each target alias owns a source reference.
 */
/*
 * The host mm is the PTE/reservation serialization domain, including across
 * Flux contexts. Fixed buckets avoid a dynamically allocated lock lifetime.
 * Different host mms can run their expensive mmap/PTE work concurrently.
 */
struct mutex flux_alias_locks[FLUX_ALIAS_LOCK_COUNT];
static atomic_t flux_alias_gate_active = ATOMIC_INIT(0);

void __init flux_alias_locks_init(void)
{
	unsigned int bucket;

	for (bucket = 0; bucket < FLUX_ALIAS_LOCK_COUNT; bucket++)
		mutex_init(&flux_alias_locks[bucket]);
}

/*
 * Source pins belong to the context's physical backing domain, independently
 * of any execution mm. Target xarray entries hold references directly; each
 * new projection resolves its source before sharing an existing page pin.
 */
struct flux_alias_mm *
flux_alias_mm_find_locked(struct flux_mm_ctx *ctx, struct mm_struct *mm)
{
	struct flux_alias_mm *alias_mm;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	list_for_each_entry(alias_mm, &ctx->alias_mms[flux_alias_bucket(mm)], node) {
		if (alias_mm->mm == mm)
			return alias_mm;
	}
	return NULL;
}

struct flux_alias_mm *
flux_alias_mm_get_locked(struct flux_mm_ctx *ctx, struct mm_struct *mm,
			     gfp_t gfp)
{
	struct flux_alias_mm *alias_mm;

	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (alias_mm)
		return alias_mm;
	alias_mm = kzalloc(sizeof(*alias_mm), gfp);
	if (!alias_mm)
		return NULL;
	alias_mm->mm = mm;
	xa_init(&alias_mm->aliases);
	list_add_tail(&alias_mm->node, &ctx->alias_mms[flux_alias_bucket(mm)]);
	return alias_mm;
}

/*
 * Consume a newly acquired pin. Resolve each new request before this lookup:
 * the registry deduplicates held page identities, never cached source VAs.
 */
struct flux_alias_source *
flux_alias_source_take(struct flux_mm_ctx *ctx,
		       struct page *page, gfp_t gfp)
{
	unsigned long index = page_to_pfn(page);
	XA_STATE(xas, &ctx->alias_sources, index);
	struct flux_alias_source *source, *fresh;
	int ret;

	/* Existing pins need only the short registry/refcount critical section. */
	xa_lock(&ctx->alias_sources);
	source = xa_load(&ctx->alias_sources, index);
	if (source)
		refcount_inc(&source->refs);
	xa_unlock(&ctx->alias_sources);
	if (source) {
		unpin_user_page(page);
		return source;
	}

	fresh = kmalloc(sizeof(*fresh), gfp);
	if (!fresh) {
		unpin_user_page(page);
		return ERR_PTR(-ENOMEM);
	}
	fresh->page = page;
	fresh->registry = &ctx->alias_sources;
	refcount_set(&fresh->refs, 1);
	/* Allocate XArray nodes outside its spinlock; NOWAIT keeps its GFP mask. */
	do {
		xas_lock(&xas);
		source = xas_load(&xas);
		if (source) {
			refcount_inc(&source->refs);
		} else {
			xas_store(&xas, fresh);
			if (!xas_error(&xas))
				source = fresh;
		}
		xas_unlock(&xas);
	} while (xas_nomem(&xas, gfp));
	ret = xas_error(&xas);
	if (ret || source != fresh) {
		unpin_user_page(page);
		kfree(fresh);
	}
	return ret ? ERR_PTR(ret) : source;
}

void flux_alias_source_put(struct flux_alias_source *source)
{
	struct xarray *registry;
	bool last;

	if (!source)
		return;
	registry = source->registry;
	/* Keep zero-ref removal atomic with lookup; unpin/free outside the lock. */
	xa_lock(registry);
	last = refcount_dec_and_test(&source->refs);
	if (last)
		__xa_erase(registry, page_to_pfn(source->page));
	xa_unlock(registry);
	if (last) {
		unpin_user_page(source->page);
		kfree(source);
	}
}

/* Consume one source reference; preserve the displaced reference for rollback. */
int flux_alias_track_source_locked(struct flux_mm_ctx *ctx,
					 struct mm_struct *mm,
					 unsigned long addr,
					 struct flux_alias_source *source,
					 gfp_t gfp,
					 struct flux_alias_source **old_source)
{
	struct flux_alias_mm *alias_mm = flux_alias_mm_get_locked(ctx, mm, gfp);
	void *old;

	if (!alias_mm || alias_mm->released) {
		flux_alias_source_put(source);
		return alias_mm ? -ENOENT : -ENOMEM;
	}
	old = xa_store(&alias_mm->aliases, addr >> PAGE_SHIFT, source, gfp);
	if (xa_is_err(old)) {
		flux_alias_source_put(source);
		return xa_err(old);
	}
	*old_source = old;
	return 0;
}

void flux_alias_restore_source_locked(struct flux_mm_ctx *ctx,
					  struct mm_struct *mm,
					  unsigned long addr,
					  struct flux_alias_source *old_source)
{
	struct flux_alias_mm *alias_mm;
	void *replaced;

	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (WARN_ON_ONCE(!alias_mm))
		return;
	if (!old_source)
		replaced = xa_erase(&alias_mm->aliases, addr >> PAGE_SHIFT);
	else
		replaced = xa_store(&alias_mm->aliases, addr >> PAGE_SHIFT,
				    old_source, GFP_NOWAIT);
	if (WARN_ON_ONCE(xa_is_err(replaced)))
		return;
	flux_alias_source_put(replaced);
}

static void flux_alias_sources_release_range_locked(struct flux_mm_ctx *ctx,
						 struct mm_struct *mm,
						 unsigned long start,
						 unsigned long end)
{
	struct flux_alias_mm *alias_mm;
	unsigned long index, last;
	struct flux_alias_source *pin;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	if (start >= end)
		return;
	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (!alias_mm)
		return;
	index = start >> PAGE_SHIFT;
	last = (end - 1) >> PAGE_SHIFT;
	while ((pin = xa_find(&alias_mm->aliases, &index, last, XA_PRESENT))) {
		xa_erase(&alias_mm->aliases, index);
		flux_alias_source_put(pin);
		if (index == last)
			break;
		index++;
	}
}

static void flux_alias_zap_range_locked(struct flux_mm_ctx *ctx,
					 struct mm_struct *mm,
					 unsigned long start,
					 unsigned long end);

/*
 * Retire only indexed projections, coalescing adjacent targets. In particular,
 * execution-mm teardown must not zap unrelated runtime/device PFNMAP mappings.
 * Revocation erases this mm's index; the alias mutex keeps the walk stable.
 */
static void flux_alias_retire_index_locked(struct flux_mm_ctx *ctx,
					  struct mm_struct *mm,
					  struct xarray *aliases, bool user_only)
{
	unsigned long index = 0, first, last;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	while (xa_find(aliases, &index, ULONG_MAX >> PAGE_SHIFT, XA_PRESENT)) {
		if (user_only && flux_kernel_alias(index << PAGE_SHIFT)) {
			index = FLUX_VMALLOC_END >> PAGE_SHIFT;
			continue;
		}
		first = index;
		last = index;
		while (last + 1 < (ULONG_MAX >> PAGE_SHIFT) &&
		       (!user_only || !flux_kernel_alias((last + 1) << PAGE_SHIFT)) &&
		       xa_load(aliases, last + 1))
			last++;
		flux_alias_zap_range_locked(ctx, mm, first << PAGE_SHIFT,
					    (last + 1) << PAGE_SHIFT);
		index = last + 1;
	}
}

static void flux_alias_mm_release_locked(struct flux_mm_ctx *ctx,
					struct mm_struct *mm)
{
	struct flux_alias_mm *alias_mm;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (!alias_mm)
		return;
	flux_projection_mm_release(alias_mm);
	flux_alias_retire_index_locked(ctx, mm, &alias_mm->aliases, false);
	alias_mm->released = true;
	/* A child construction can still own a gate on this execution mm. */
	if (alias_mm->gate_refs)
		return;
	list_del(&alias_mm->node);
	xa_destroy(&alias_mm->aliases);
	kfree(alias_mm);
}

void flux_alias_release_mm(struct flux_mm_ctx *ctx, struct mm_struct *mm)
{
	u64 start = flux_fork_stats_start();

	mutex_lock(flux_alias_mm_lock(mm));
	flux_alias_mm_release_locked(ctx, mm);
	mutex_unlock(flux_alias_mm_lock(mm));
	flux_fork_stats_end(FLUX_FORK_STAT_ALIAS_RELEASE, start);
}

int flux_alias_munmap_range(struct flux_mm_ctx *ctx, unsigned long start,
			    size_t len)
{
	struct mm_struct *mm = current->mm;
	unsigned long end;
	int ret;

	if (!mm || !len || start > ULONG_MAX - len)
		return -EINVAL;
	end = start + len;
	mutex_lock(flux_alias_mm_lock(mm));
	ret = vm_munmap(start, len);
	if (!ret)
		flux_alias_sources_release_range_locked(ctx, mm, start, end);
	mutex_unlock(flux_alias_mm_lock(mm));
	return ret;
}

static int flux_alias_gate_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d", atomic_read(&flux_alias_gate_active));
}

static const struct kernel_param_ops flux_alias_gate_count_ops = {
	.get = flux_alias_gate_count_get,
};
module_param_cb(fork_alias_gate_active, &flux_alias_gate_count_ops, NULL, 0444);

static pgprot_t flux_alias_pgprot(pgprot_t prot, unsigned int pkey)
{
	return __pgprot((pgprot_val(prot) & ~_PAGE_PKEY_MASK) |
			((u64)pkey << _PAGE_BIT_PKEY_BIT0));
}

/*
 * Validate every alias before releasing the exact mmap/alias lock domain which
 * installed it.  NOWAIT compares against the original request so a transient
 * gate-downgraded alias cannot complete a writable fault.  The caller passes
 * the effective writable state for blocking operations.
 */
static int
flux_alias_verify_postcondition_locked(struct vm_area_struct *vma,
				       unsigned long uva, unsigned long pfn,
				       bool requested_writable,
				       bool executable, unsigned int pkey)
{
	struct flux_follow_pfnmap_args follow = {
		.vma = vma,
		.address = uva,
	};
	pte_t host_pte;
	int ret;

	ret = flux_follow_pfnmap_start(&follow);
	if (ret)
		return -EAGAIN;

	host_pte = ptep_get(follow.ptep);
	ret = pte_present(host_pte) && pte_special(host_pte) &&
	      pte_pfn(host_pte) == pfn &&
	      pte_write(host_pte) == requested_writable &&
	      pte_exec(host_pte) == executable &&
	      pte_flags_pkey(pte_val(host_pte)) == pkey ? 0 : -EAGAIN;
	flux_follow_pfnmap_end(&follow);
	return ret;
}

/*
 * A kernel-vmalloc fault reads the Flux PTE before entering this module.  A
 * concurrent pte_clear() can publish non-present and finish its cross-mm zap
 * before the delayed fault acquires the alias mutex.  Validate after install,
 * in the same lock domain, so that stale request cannot resurrect the old PFN.
 */
static int
flux_alias_verify_pte_locked(struct mm_struct *mm,
			     const struct flux_alias_args *a)
{
	__u64 current_pte;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	if (!(a->flags & FLUX_ALIAS_F_CHECK_PTE))
		return 0;
	if (copy_from_user_nofault(
			&current_pte,
			(const void __user *)(unsigned long)a->expected_pte_addr,
			sizeof(current_pte)))
		return -EAGAIN;
	return ((current_pte ^ a->expected_pte) & a->expected_pte_mask) ?
		-EAGAIN : 0;
}

/*
 * Install/verify one host projection with source ownership already held by
 * the caller.  No reservation, GUP or source bookkeeping belongs here.
 * The caller owns both locks and decides how to retire/restore source refs.
 * This is not a nonblocking interface: insertion may allocate page tables.
 */
int flux_alias_install_pfn_locked(struct flux_mm_ctx *ctx,
					struct vm_area_struct *vma,
					unsigned long uva, unsigned long pfn,
					const struct flux_alias_args *a,
					bool *alias_absent)
{
	struct flux_follow_pfnmap_args follow = {
		.vma = vma,
		.address = uva,
	};
	struct mm_struct *mm = vma->vm_mm;
	struct flux_alias_mm *alias_mm = flux_alias_mm_find_locked(ctx, mm);
	bool shared = a->flags & FLUX_ALIAS_F_SHARED;
	bool nowait = a->flags & FLUX_ALIAS_F_NOWAIT;
	bool writable = (a->prot & PROT_WRITE) &&
		(shared || !alias_mm || !alias_mm->gate_refs ||
		 flux_kernel_alias(uva));
	bool executable = a->prot & PROT_EXEC;
	bool required_writable = nowait ? !!(a->prot & PROT_WRITE) : writable;
	bool alias_present = false;
	bool alias_matches = false;
	pgprot_t prot;
	vm_fault_t vmf;
	int ret;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	mmap_assert_write_locked(mm);
	if (WARN_ON_ONCE(!alias_mm))
		return -EFAULT;
	if (writable)
		prot = executable ? PAGE_SHARED_EXEC : PAGE_SHARED;
	else
		prot = executable ? PAGE_READONLY_EXEC : PAGE_READONLY;
	prot = flux_alias_pgprot(prot, a->pkey);

	/* Reservation construction owns VMA flags, never the PTE installer. */
	if (!(vma->vm_flags & VM_PFNMAP))
		return -EAGAIN;
	if (!flux_follow_pfnmap_start(&follow)) {
		alias_present = true;
		alias_matches = follow.pfn == pfn &&
			follow.writable == writable &&
			pte_exec(__pte(pgprot_val(follow.pgprot))) ==
				executable &&
			pte_flags_pkey(pgprot_val(follow.pgprot)) == a->pkey;
		flux_follow_pfnmap_end(&follow);
	}
	/* Only replacement needs revocation. Empty PFNMAP slots, including
	 * a child's first fault, have no PTE/TLB state to discard. The caller
	 * retains the old source pin across replacement and validation.
	 */
	if (!alias_matches) {
		if (alias_present)
			zap_vma_ptes(vma, uva, PAGE_SIZE);
		*alias_absent = true;
		vmf = vmf_insert_pfn_prot(vma, uva, pfn, prot);
		if (vmf & VM_FAULT_ERROR) {
			pr_warn_ratelimited(
				"alias insert pid=%d uva=%#lx pfn=%#lx vmf=%#x failed\n",
				task_pid_nr(current), uva, pfn, vmf);
			return -EFAULT;
		}
	}
	/* Both identical and newly installed PTEs have the same contract. */
	ret = flux_alias_verify_postcondition_locked(
		vma, uva, pfn, required_writable, executable, a->pkey);
	if (!ret)
		ret = flux_alias_verify_pte_locked(mm, a);
	if (ret) {
		zap_vma_ptes(vma, uva, PAGE_SIZE);
		*alias_absent = true;
		return ret;
	}
	/* XArray marks are a sparse bitmap of private writable projections. */
	if (writable && !shared && !flux_kernel_alias(uva))
		xa_set_mark(&alias_mm->aliases, uva >> PAGE_SHIFT, XA_MARK_0);
	else
		xa_clear_mark(&alias_mm->aliases, uva >> PAGE_SHIFT, XA_MARK_0);
	*alias_absent = false;
	return 0;
}

#define FLUX_ALIAS_SOURCE_BATCH 32

/*
 * Return a positive pinned prefix or an error. Consume short blocking GUP
 * results before preparing the next prefix. NOWAIT stays single-page and
 * never uses the blocking fast-GUP fallback.
 */
static long flux_alias_pin_source_pages(struct mm_struct *mm,
				       unsigned long addr, unsigned int nr,
				       bool shared, bool nowait,
				       struct page **pages)
{
	unsigned int gup_flags =
		FOLL_LONGTERM | (shared ? 0 : FOLL_WRITE);
	int locked = 1;
	long pinned;

	if (!nowait) {
		pinned = pin_user_pages_fast(addr, nr, gup_flags, pages);
		return pinned > 0 ? pinned : -EFAULT;
	}
	if (WARN_ON_ONCE(nr != 1))
		return -EINVAL;
	if (!mmap_read_trylock(mm))
		return -EAGAIN;
	pinned = pin_user_pages_remote(mm, addr, 1,
				       gup_flags | FOLL_NOWAIT, pages, &locked);
	if (locked)
		mmap_read_unlock(mm);
	return pinned == 1 ? 1 : (pinned < 0 ? pinned : -EAGAIN);
}

static int flux_alias_pages_impl(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_alias_args a;
	struct mm_struct *mm = current->mm;
	struct flux_alias_source *old_source = NULL;
	struct flux_alias_source *source;
	struct page *source_pages[FLUX_ALIAS_SOURCE_BATCH];
	struct flux_alias_source *repeat_source = NULL;
	bool repeat;
	unsigned int pinned = 0, cursor = 0;
	unsigned long source_uva = 0;
	bool source_tracked = false;
	bool nowait;
	bool alias_absent = false;
	bool mmap_locked = false;
	unsigned long i;
	int ret;

	if (!mm)
		return -EINVAL;
	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	repeat = a.flags & FLUX_ALIAS_F_REPEAT_SOURCE;
	if (!a.nr || a.nr > (1UL << 20) ||
	    a.user_va > ULONG_MAX - a.nr * PAGE_SIZE ||
	    a.src_va > ULONG_MAX - (repeat ? 1 : a.nr) * PAGE_SIZE || a.pkey > 15 ||
	    (a.flags & ~(FLUX_ALIAS_F_SHARED | FLUX_ALIAS_F_NOWAIT |
			 FLUX_ALIAS_F_CHECK_PTE | FLUX_ALIAS_F_REPEAT_SOURCE)) || a.pad)
		return -EINVAL;
	if (a.flags & FLUX_ALIAS_F_CHECK_PTE) {
		if (a.nr != 1 || !a.expected_pte_addr || !a.expected_pte_mask ||
		    (a.expected_pte_addr & (sizeof(__u64) - 1)))
			return -EINVAL;
	} else if (a.expected_pte_addr || a.expected_pte ||
		   a.expected_pte_mask) {
		return -EINVAL;
	}
	nowait = a.flags & FLUX_ALIAS_F_NOWAIT;
	/* Repeated-source preparation is normal-context only. */
	if (repeat && nowait)
		return -EOPNOTSUPP;
	if (repeat) {
		struct page *page;

		ret = flux_alias_pin_source_pages(mm, a.src_va, 1,
				 a.flags & FLUX_ALIAS_F_SHARED, false, &page);
		if (ret > 0) {
			mutex_lock(flux_alias_mm_lock(mm));
			repeat_source = flux_alias_source_take(ctx, page, GFP_KERNEL);
			if (IS_ERR(repeat_source)) {
				ret = PTR_ERR(repeat_source);
				repeat_source = NULL;
			}
			mutex_unlock(flux_alias_mm_lock(mm));
		}
		if (ret < 0)
			return ret;
	}
	for (i = 0; i < a.nr;) {
		unsigned int want = nowait ? 1 :
			min_t(unsigned long, FLUX_ALIAS_SOURCE_BATCH, a.nr - i);

		pinned = 0;
		cursor = 0;
		source_tracked = false;
		old_source = NULL;
		alias_absent = false;
		/*
		 * GUP owns temporary pins before registry publication. Resolve the
		 * source under its native mmap/GUP locks; a host backing-page fault
		 * must not serialize every execution mm on the alias registry.
		 */
		if (repeat) {
			pinned = want;
		} else {
			ret = flux_alias_pin_source_pages(mm, a.src_va + i * PAGE_SIZE,
						 want, a.flags & FLUX_ALIAS_F_SHARED,
						 nowait, source_pages);
			if (ret < 0)
				return ret;
			pinned = ret;
		}
		if (nowait) {
			if (!mutex_trylock(flux_alias_mm_lock(mm))) {
				/* NOWAIT is single-page and cannot use repeat_source. */
				unpin_user_page(source_pages[0]);
				return -EAGAIN;
			}
		} else {
			mutex_lock(flux_alias_mm_lock(mm));
		}

		for (cursor = 0; cursor < pinned; cursor++, i++) {
			unsigned long uva = a.user_va + i * PAGE_SIZE;
			unsigned long pfn;
			struct vm_area_struct *vma;
			bool reservation_prepared = false;
			bool shared = a.flags & FLUX_ALIAS_F_SHARED;
			long reserve;

			old_source = NULL;
			source_uva = uva;
			source_tracked = false;
			alias_absent = false;

			if (repeat) {
				source = repeat_source;
				refcount_inc(&source->refs);
			} else {
				source = flux_alias_source_take(ctx,
					source_pages[cursor],
					nowait ? GFP_NOWAIT : GFP_KERNEL);
				source_pages[cursor] = NULL;
				if (IS_ERR(source)) {
					ret = PTR_ERR(source);
					goto out_unlock_alias;
				}
			}
			pfn = page_to_pfn(source->page);
			ret = flux_alias_track_source_locked(ctx, mm, uva, source,
				nowait ? GFP_NOWAIT : GFP_KERNEL, &old_source);
			if (ret)
				goto out_unlock_alias;
			source_tracked = true;

	retry_vma:
			/* Reuse the host lock within this bounded source batch.
			 * GUP ran before the lock; reservation must drop it.
			 */
			if (!mmap_locked) {
				mmap_write_lock(mm);
				mmap_locked = true;
			}
			vma = find_vma(mm, uva);
			if ((!vma || uva < vma->vm_start ||
			     !(vma->vm_flags & VM_PFNMAP)) && !reservation_prepared) {
				unsigned long reserve_len = PAGE_SIZE;
				bool replace = true;

				/* Prepare the remaining pinned prefix in an empty hole.
				 * Never cross an existing VMA or change NOWAIT admission.
				 */
				if (!nowait && (!vma || uva < vma->vm_start)) {
					reserve_len = (unsigned long)(pinned - cursor) << PAGE_SHIFT;
					if (vma)
						reserve_len = min(reserve_len, vma->vm_start - uva);
					replace = false;
				}
				mmap_write_unlock(mm);
				mmap_locked = false;
				reserve = flux_alias_reserve_range(uva, reserve_len, shared, replace);
				/* A concurrent mapper may have occupied the inspected hole. */
				if (!replace && reserve == -EEXIST) {
					ret = -EAGAIN;
					goto out_unlock_alias;
				}
				if (IS_ERR_VALUE(reserve) || (unsigned long)reserve != uva) {
					if (!IS_ERR_VALUE(reserve))
						vm_munmap(reserve, reserve_len);
					ret = IS_ERR_VALUE(reserve) ? (int)reserve : -EFAULT;
					pr_warn_ratelimited(
						"alias reserve pid=%d uva=%#lx failed ret=%d raw=%#lx\n",
						task_pid_nr(current), uva, ret,
						(unsigned long)reserve);
					goto out_unlock_alias;
				}
				reservation_prepared = true;
				goto retry_vma;
			}
			if (!vma || uva < vma->vm_start) {
				ret = -EFAULT;
				goto out_unlock_alias;
			}
			if ((vma->vm_flags & VM_PFNMAP) &&
			    !!(vma->vm_flags & VM_SHARED) != shared) {
				ret = -EINVAL;
				goto out_unlock_alias;
			}
			ret = flux_alias_install_pfn_locked(ctx, vma, uva, pfn, &a,
							    &alias_absent);
			if (ret)
				goto out_unlock_alias;
			flux_alias_source_put(old_source);
			old_source = NULL;
			source_tracked = false;

		}
		mmap_write_unlock(mm);
		mmap_locked = false;
		pinned = 0;
		mutex_unlock(flux_alias_mm_lock(mm));
	}

	if (repeat_source) {
		mutex_lock(flux_alias_mm_lock(mm));
		flux_alias_source_put(repeat_source);
		mutex_unlock(flux_alias_mm_lock(mm));
	}
	return 0;

out_unlock_alias:
	if (mmap_locked)
		mmap_write_unlock(mm);
	if (source_tracked) {
		if (alias_absent) {
			flux_alias_restore_source_locked(ctx, mm, source_uva, NULL);
			flux_alias_source_put(old_source);
		} else {
			flux_alias_restore_source_locked(ctx, mm, source_uva, old_source);
		}
	}
	/* Unconsumed prepared pins still belong to this batch. */
	for (; !repeat && cursor < pinned; cursor++) {
		if (source_pages[cursor])
			unpin_user_page(source_pages[cursor]);
	}
	if (repeat_source)
		flux_alias_source_put(repeat_source);
	mutex_unlock(flux_alias_mm_lock(mm));
	return ret;
}

/**
 * flux_alias_pages() - install Linux PFN aliases in current's mm
 * @ctx: owning Flux MM context
 * @arg: userspace pointer to struct flux_alias_args
 *
 * Return: 0 on success, or a negative errno.
 */
int flux_alias_pages(struct flux_mm_ctx *ctx, unsigned long arg)
{
	u64 start = flux_fork_stats_start();
	int ret = flux_alias_pages_impl(ctx, arg);

	flux_fork_stats_end(FLUX_FORK_STAT_ALIAS_INSTALL, start);
	return ret;
}

/**
 * flux_alias_range_has_pkey() - validate the effective pkey of an SKAS alias
 * @vma: PFNMAP reservation covering the range
 * @start: first address to validate
 * @end: exclusive end address
 * @pkey: required protection key
 *
 * SKAS aliases keep a PROT_NONE, pkey-0 reservation VMA and install special
 * PTEs with the Flux mapping's actual permissions and pkey.  Consequently,
 * vma_pkey() does not describe the effective key.  Empty reservation pages
 * are safe: they expose no mapping and will be populated with an explicit
 * pkey on the next Flux fault.
 *
 * The caller must hold @vma->vm_mm's mmap lock.
 *
 * Return: %true when @vma is a user SKAS alias and every installed PTE in the
 * range uses @pkey.
 */
bool flux_alias_range_has_pkey(struct vm_area_struct *vma,
			       unsigned long start, unsigned long end, int pkey)
{
	unsigned long addr;
	const vm_flags_t alias_flags = VM_PFNMAP | VM_DONTEXPAND |
				       VM_DONTDUMP;

	if (!vma || start < vma->vm_start || end > vma->vm_end || start >= end ||
	    flux_kernel_alias(start) ||
	    (vma->vm_flags & alias_flags) != alias_flags)
		return false;

	for (addr = start & PAGE_MASK; addr < end; addr += PAGE_SIZE) {
		struct flux_follow_pfnmap_args follow = {
			.vma = vma,
			.address = addr,
		};
		pte_t pte;

		if (flux_follow_pfnmap_start(&follow))
			continue;
		pte = ptep_get(follow.ptep);
		if (!pte_special(pte) || pte_flags_pkey(pte_val(pte)) != pkey) {
			flux_follow_pfnmap_end(&follow);
			return false;
		}
		flux_follow_pfnmap_end(&follow);
	}

	return true;
}

/**
 * flux_rekey_aliases() - atomically move an existing alias range to a pkey
 * @arg: userspace pointer to struct flux_rekey_alias_args
 *
 * Return: 0 on success, or a negative errno.
 */
int flux_rekey_aliases(unsigned long arg)
{
	struct flux_rekey_alias_args a;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long addr;
	unsigned long end;
	int ret = 0;

	if (!mm)
		return -EINVAL;
	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	if (!a.nr || a.nr > (1UL << 20) || a.pkey > 15 ||
	    a.pad ||
	    a.user_va > ULONG_MAX - a.nr * PAGE_SIZE)
		return -EINVAL;
	end = a.user_va + a.nr * PAGE_SIZE;
	if (unlikely(!flux_flush_tlb_mm_range)) {
		flux_flush_tlb_mm_range =
			(void *)flux_lookup_symbol("flush_tlb_mm_range");
		if (!flux_flush_tlb_mm_range)
			return -ENOENT;
	}

	mutex_lock(flux_alias_mm_lock(mm));
	mmap_write_lock(mm);
	for (addr = a.user_va; addr < end; addr += PAGE_SIZE) {
		struct flux_follow_pfnmap_args follow = {
			.address = addr,
		};

		vma = find_vma(mm, addr);
		if (!vma || addr < vma->vm_start || !(vma->vm_flags & VM_PFNMAP)) {
			ret = -EFAULT;
			break;
		}
		follow.vma = vma;
		if (flux_follow_pfnmap_start(&follow)) {
			ret = -EFAULT;
			break;
		}
		if (unlikely(!pte_special(ptep_get(follow.ptep))))
			ret = -EFAULT;
		flux_follow_pfnmap_end(&follow);
		if (ret)
			break;
	}
	if (ret)
		goto out_unlock;

	for (addr = a.user_va; addr < end; addr += PAGE_SIZE) {
		struct flux_follow_pfnmap_args follow = {
			.address = addr,
		};
		pte_t pte;

		vma = find_vma(mm, addr);
		follow.vma = vma;
		if (WARN_ON_ONCE(flux_follow_pfnmap_start(&follow))) {
			ret = -EFAULT;
			break;
		}
		pte = ptep_get(follow.ptep);
		/*
		 * Linux owns access permissions, including the fork COW gate.
		 * Rekeying must never promote a write-protected projection.
		 */
		pte = pte_clear_flags(pte, _PAGE_PKEY_MASK);
		pte = pte_set_flags(pte, (pteval_t)a.pkey << _PAGE_BIT_PKEY_BIT0);
		set_pte_at(mm, addr, follow.ptep, pte);
		flux_follow_pfnmap_end(&follow);
	}
	if (unlikely(ret)) {
		/* Validation above and the alias mutex make this unreachable. */
		pr_err("partial alias rekey at %#lx: %d\n", addr, ret);
		BUG();
	}
	flux_flush_tlb_mm_range(mm, a.user_va, end, PAGE_SHIFT, false);

out_unlock:
	mmap_write_unlock(mm);
	mutex_unlock(flux_alias_mm_lock(mm));
	return ret;
}

static void flux_alias_zap_range_locked(struct flux_mm_ctx *ctx,
					 struct mm_struct *mm,
					 unsigned long start,
					 unsigned long end)
{
	struct vm_area_struct *vma;
	struct flux_alias_mm *alias_mm;
	unsigned long index = start >> PAGE_SHIFT;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	if (start >= end)
		return;
	/* Every installed projection owns an indexed source under this lock. */
	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	/* Even an empty alias index must synchronize software table walkers. */
	if (alias_mm)
		flux_projection_invalidate_locked(alias_mm, start, end);
	if (!alias_mm || !xa_find(&alias_mm->aliases, &index,
				 (end - 1) >> PAGE_SHIFT, XA_PRESENT))
		return;
	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	while (vma && vma->vm_start < end) {
		unsigned long first = max(start, vma->vm_start);
		unsigned long last = min(end, vma->vm_end);

		if ((vma->vm_flags & VM_PFNMAP) && first < last)
			zap_vma_ptes(vma, first, last - first);
		vma = find_vma(mm, vma->vm_end);
	}
	mmap_write_unlock(mm);
	flux_alias_sources_release_range_locked(ctx, mm, start, end);
}

/**
 * flux_unalias_pages() - remove stale aliases from a Flux process slot
 * @ctx: MM context containing the target process slot
 * @arg: userspace pointer to struct flux_unalias_args
 *
 * Flux page-table updates can be performed by a kernel worker while another
 * Flux mm is inactive (KSM is the important example).  Its hardware-visible
 * host alias then still points at the old PFN and will not fault, so the Flux
 * never gets a chance to rebuild it.  Zap only the requested special aliases
 * in that mm; the normal synchronous-fault path recreates them from the new
 * Flux PTE when the task next accesses the page.
 *
 * Return: 0 on success, or a negative errno.
 */
int flux_unalias_pages(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_unalias_args a;
	struct mm_struct *mm;
	struct flux_proc *proc;
	unsigned long start, end;

	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	if (!a.nr || a.nr > (1UL << 20) ||
	    a.user_va > ULONG_MAX - a.nr * PAGE_SIZE)
		return -EINVAL;

	start = a.user_va & PAGE_MASK;
	end = start + a.nr * PAGE_SIZE;
	proc = flux_proc_get_by_key(ctx, a.proc_key);
	if (!proc)
		return -ENOENT;
	mm = READ_ONCE(proc->mm);
	if (!mm) {
		flux_proc_put(ctx, a.proc_key, proc);
		return -ENOENT;
	}

	mutex_lock(flux_alias_mm_lock(mm));
	flux_alias_zap_range_locked(ctx, mm, start, end);
	mutex_unlock(flux_alias_mm_lock(mm));
	flux_proc_put(ctx, a.proc_key, proc);
	return 0;
}

/*
 * The last LibOS mm user revokes application projections before freeing its
 * PTEs and pages. Use the ownership index once, instead of issuing a host
 * ioctl and mmap lock acquisition for every (usually untouched) LibOS VMA.
 * Kernel-vmalloc aliases can still serve the exiting task and remain until
 * the host process slot is released or a normal kernel invalidation occurs.
 */
static int flux_retire_user_mm(struct flux_mm_ctx *ctx, int proc_key, bool exiting)
{
	struct flux_alias_mm *alias_mm;
	struct flux_proc *proc;
	struct mm_struct *mm;
	int ret = 0;
	u64 start;

	if (proc_key <= 0)
		return -EINVAL;
	proc = flux_proc_get_by_key(ctx, proc_key);
	if (!proc)
		return -ENOENT;
	start = flux_fork_stats_start();
	mm = READ_ONCE(proc->mm);
	if (!mm) {
		ret = -ENOENT;
		goto out_put;
	}
	mutex_lock(flux_alias_mm_lock(mm));
	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (alias_mm) {
		if (exiting)
			flux_projection_mm_release(alias_mm);
		else
			flux_projection_invalidate_locked(alias_mm, 0, ULONG_MAX);
		flux_alias_retire_index_locked(ctx, mm, &alias_mm->aliases, true);
	}
	mutex_unlock(flux_alias_mm_lock(mm));
out_put:
	flux_proc_put(ctx, proc_key, proc);
	flux_fork_stats_end(FLUX_FORK_STAT_ALIAS_UNALIAS_USER, start);
	return ret;
}

int flux_unalias_user_mm(struct flux_mm_ctx *ctx, int proc_key)
{
	return flux_retire_user_mm(ctx, proc_key, true);
}

int flux_flush_user_mm(struct flux_mm_ctx *ctx, int proc_key)
{
	return flux_retire_user_mm(ctx, proc_key, false);
}

/**
 * flux_unalias_kernel_pages() - invalidate a Flux kernel alias in every host mm
 * @ctx: MM context containing every host mm with pinned aliases
 * @arg: userspace pointer to struct flux_kernel_unalias_args
 *
 * A Flux vmalloc PTE is shared by all Flux mms, but each SKAS host mm owns
 * an independent PFNMAP alias.  Once Flux publishes a non-present PTE,
 * zap the corresponding host alias everywhere before the backing page can be
 * reused. Participating mm state remains registered until explicit teardown.
 *
 * Return: 0 on success, or a negative errno.
 */
int flux_unalias_kernel_pages(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_kernel_unalias_args a;
	struct flux_alias_mm *alias_mm;
	unsigned long start, end;

	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	if (!a.nr || a.nr > (1UL << 20) ||
	    (a.user_va & ~PAGE_MASK) ||
	    a.user_va > ULONG_MAX - a.nr * PAGE_SIZE)
		return -EINVAL;

	start = a.user_va;
	end = start + a.nr * PAGE_SIZE;
	if (start < FLUX_VMALLOC_START ||
	    end > FLUX_VMALLOC_END)
		return -EINVAL;

	/*
	 * The Flux PTE was cleared before this call. A new installer behind an
	 * already-visited bucket must fail its expected-PTE validation, under
	 * that same bucket lock. Do not hold two buckets at the same time.
	 */
	for (unsigned int bucket = 0; bucket < FLUX_ALIAS_LOCK_COUNT; bucket++) {
		mutex_lock(&flux_alias_locks[bucket]);
		list_for_each_entry(alias_mm, &ctx->alias_mms[bucket], node)
			flux_alias_zap_range_locked(ctx, alias_mm->mm, start, end);
		mutex_unlock(&flux_alias_locks[bucket]);
	}
	return 0;
}

/*
 * The host child has no aliases (VM_DONTCOPY). Parent private aliases must
 * become read-only before LibOS Linux establishes COW. The paired fork
 * completion uses this host write-protection/TLB synchronization directly;
 * unchanged parent aliases remain installed throughout the transaction.
 *
 * Alias VMAs can merge and therefore contain more than one PTE.  Protecting
 * only vm_start leaves the remaining aliases writable across fork and lets
 * the parent modify COW-shared Flux pages.
 */
static int flux_alias_writeprotect_ptes_locked(struct flux_alias_mm *alias_mm)
{
	struct mm_struct *mm = alias_mm->mm;
	struct vm_area_struct *vma = NULL;
	struct flux_alias_source *source;
	unsigned long index, first = ULONG_MAX, last = 0;

	lockdep_assert_held(flux_alias_mm_lock(mm));
	if (!xa_marked(&alias_mm->aliases, XA_MARK_0))
		return 0;
	if (unlikely(!flux_flush_tlb_mm_range)) {
		flux_flush_tlb_mm_range =
			(void *)flux_lookup_symbol("flush_tlb_mm_range");
		if (!flux_flush_tlb_mm_range) {
			pr_err("failed to resolve flush_tlb_mm_range symbol\n");
			return -ENOENT;
		}
	}
	mmap_write_lock(mm);
	/* Only projections installed writable since the last gate need a walk. */
	xa_for_each_marked(&alias_mm->aliases, index, source, XA_MARK_0) {
		unsigned long addr = index << PAGE_SHIFT;
		struct flux_follow_pfnmap_args follow = { .address = addr };

		if (flux_kernel_alias(addr))
			continue;
		if (!vma || addr >= vma->vm_end)
			vma = find_vma(mm, addr);
		if (!vma || addr < vma->vm_start ||
		    !(vma->vm_flags & VM_PFNMAP) || (vma->vm_flags & VM_SHARED))
			continue;
		follow.vma = vma;
		if (flux_follow_pfnmap_start(&follow))
			continue;
		if (follow.writable) {
			ptep_set_wrprotect(mm, addr, follow.ptep);
			first = min(first, addr);
			last = addr + PAGE_SIZE;
		}
		flux_follow_pfnmap_end(&follow);
		xa_clear_mark(&alias_mm->aliases, index, XA_MARK_0);
	}
	if (last)
		flux_flush_tlb_mm_range(mm, first, last, PAGE_SHIFT, false);
	mmap_write_unlock(mm);
	return 0;
}

/*
 * Fork construction and installed projections share one execution-mm state.
 * The first gate holds mm_users until the last gate closes; slot retirement
 * revokes projections immediately but keeps this state until that close.
 */
int flux_alias_gate_acquire_mm(struct flux_mm_ctx *ctx, struct mm_struct *mm)
{
	struct flux_alias_mm *alias_mm;
	bool put_mm = false;
	int ret;
	u64 start = flux_fork_stats_start();

	mutex_lock(flux_alias_mm_lock(mm));
	alias_mm = flux_alias_mm_get_locked(ctx, mm, GFP_KERNEL);
	if (!alias_mm || alias_mm->released) {
		ret = alias_mm ? -ENOENT : -ENOMEM;
		goto out_unlock;
	}
	if (!alias_mm->gate_refs++)
		mmget(mm);
	atomic_inc(&flux_alias_gate_active);
	ret = flux_alias_writeprotect_ptes_locked(alias_mm);
	if (ret) {
		atomic_dec(&flux_alias_gate_active);
		put_mm = !--alias_mm->gate_refs;
	}
out_unlock:
	mutex_unlock(flux_alias_mm_lock(mm));
	if (put_mm)
		mmput(mm);
	flux_fork_stats_end(FLUX_FORK_STAT_ALIAS_GATE, start);
	return ret;
}

int flux_alias_gate_release(struct flux_mm_ctx *ctx, struct mm_struct *mm)
{
	struct flux_alias_mm *alias_mm;
	bool put_mm;

	mutex_lock(flux_alias_mm_lock(mm));
	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (WARN_ON_ONCE(!alias_mm || !alias_mm->gate_refs)) {
		mutex_unlock(flux_alias_mm_lock(mm));
		return -EINVAL;
	}
	/* Install honors the gate; rekey preserves permissions. No second walk. */
	atomic_dec(&flux_alias_gate_active);
	put_mm = !--alias_mm->gate_refs;
	if (put_mm && alias_mm->released)
		flux_alias_mm_release_locked(ctx, mm);
	mutex_unlock(flux_alias_mm_lock(mm));
	if (put_mm)
		mmput(mm);
	return 0;
}

/**
 * flux_fork_alias_begin() - complete a prepared fork's host address space
 * @ctx: MM context containing the prepared process slot
 * @proc_key: nonzero key returned by flux_copy_mm()
 *
 * The parent alias gate already covers LibOS COW. Delay this expensive host
 * copy until LibOS dup_mmap has accepted every VMA and copied its PTEs. Host
 * alias reservations are VM_DONTCOPY, so the clone contains infrastructure
 * only; application aliases are rebuilt from the child's LibOS page tables.
 *
 * Return: 0 after publishing the host mm, or a negative errno. Failure leaves
 * the prepared slot for RELEASE_MM to discard together with its alias gate.
 */
int flux_fork_alias_begin(struct flux_mm_ctx *ctx, int proc_key)
{
	struct mm_struct *oldmm = NULL, *mm;
	struct flux_proc *proc;
	int ret = -EINVAL;
	u64 start;

	if (proc_key <= 0)
		return -EINVAL;
	proc = flux_proc_get_by_key(ctx, proc_key);
	if (!proc)
		return -ENOENT;

	xa_lock(&ctx->procs);
	if (!READ_ONCE(proc->released) && proc->for_fork &&
	    !proc->mm && proc->alias_gate_mm &&
	    atomic_read(&proc->alias_state) == FLUX_ALIAS_STATE_PENDING) {
		atomic_set(&proc->alias_state, FLUX_ALIAS_STATE_COPYING);
		oldmm = proc->alias_gate_mm;
		/* RELEASE_MM may claim the gate while host duplication sleeps. */
		mmget(oldmm);
	}
	xa_unlock(&ctx->procs);
	if (!oldmm)
		goto out_put;

	start = flux_fork_stats_start();
	mm = flux_fork_mm_get(ctx, oldmm);
	flux_fork_stats_end(FLUX_FORK_STAT_CACHE_GET, start);
	if (!mm) {
		start = flux_fork_stats_start();
		mm = flux_dup_mm(current, oldmm);
		flux_fork_stats_end(FLUX_FORK_STAT_HOST_DUP_MM, start);
	}
	if (mm) {
		mmgrab(oldmm);
		proc->fork_parent = oldmm;
	}
	mmput(oldmm);
	xa_lock(&ctx->procs);
	if (READ_ONCE(proc->released) ||
	    atomic_read(&proc->alias_state) != FLUX_ALIAS_STATE_COPYING) {
		ret = -ENOENT;
	} else if (!mm) {
		atomic_set(&proc->alias_state, FLUX_ALIAS_STATE_PENDING);
		ret = -ENOMEM;
	} else {
		WRITE_ONCE(proc->mm, mm);
		atomic_set(&proc->alias_state, FLUX_ALIAS_STATE_ACTIVE);
		mm = NULL; /* Transfer the clone's ownership to the slot. */
		ret = 0;
	}
	xa_unlock(&ctx->procs);
	if (mm)
		mmput(mm);
out_put:
	flux_proc_put(ctx, proc_key, proc);
	return ret;
}

/**
 * flux_fork_alias_end() - finish Flux fork and release its alias gate
 * @ctx: MM context containing the copied process slot
 * @proc_key: nonzero key passed to flux_fork_alias_begin()
 *
 * Return: 0 when the active-to-none transition and gate release succeed, or
 * a negative errno.
 */
int flux_fork_alias_end(struct flux_mm_ctx *ctx, int proc_key)
{
	struct mm_struct *gate_mm;
	struct flux_proc *proc;
	int old_state;
	int ret;

	if (proc_key <= 0)
		return -EINVAL;
	proc = flux_proc_get_by_key(ctx, proc_key);
	if (!proc)
		return -ENOENT;
	/* RELEASE_MM uses this same lock to claim state and gate together. */
	xa_lock(&ctx->procs);
	old_state = atomic_cmpxchg(&proc->alias_state,
				   FLUX_ALIAS_STATE_ACTIVE,
				   FLUX_ALIAS_STATE_NONE);
	if (old_state != FLUX_ALIAS_STATE_ACTIVE) {
		xa_unlock(&ctx->procs);
		flux_proc_put(ctx, proc_key, proc);
		return -EINVAL;
	}

	gate_mm = xchg(&proc->alias_gate_mm, NULL);
	xa_unlock(&ctx->procs);
	/* Gate release and final puts may sleep; never hold xa_lock here. */
	if (WARN_ON_ONCE(!gate_mm))
		ret = -EINVAL;
	else
		ret = flux_alias_gate_release(ctx, gate_mm);
	flux_proc_put(ctx, proc_key, proc);
	return ret;
}
