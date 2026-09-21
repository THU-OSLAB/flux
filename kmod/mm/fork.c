// SPDX-License-Identifier: GPL-2.0
/*
 * Bounded reuse of inactive host execution address spaces.
 *
 * A slot owns an mm_users reference and an mm_count reference to the parent.
 * Only a fully retired process MM with no task/gate references can enter.
 * Alias PTE retirement and TLB invalidation precede insertion. A hit preserves
 * coherent runtime mappings, after checking the same parent's VMAs and COW
 * pages under its MM/VMA locks. Any uncertainty falls back to dup_mm().
 */
#define pr_fmt(fmt) "flux_mm: " fmt

#include <linux/hugetlb.h>
#include <linux/rmap.h>
#include <linux/sched/coredump.h>
#include <linux/shmem_fs.h>
#include <linux/sizes.h>
#include <linux/userfaultfd_k.h>

#include "fork_stats.h"
#include "internal.h"

#define FLUX_FORK_MM_RETRY_INTERVAL 32
#define FLUX_FORK_RESERVATION_PAGES 512
/* Preserve the aggregate bounds of the original four-entry cache. */
#define FLUX_FORK_CACHE_PGTABLE_BYTES SZ_8M
#define FLUX_FORK_CACHE_VMAS 1024
#define FLUX_FORK_CACHE_PRIVATE_PAGES 256
#define FLUX_FORK_CACHE_RESERVATION_PAGES 2048

/* Opt-in diagnostics avoid walking tables in performance builds/runs. */
static void flux_fork_pmd_observe(struct mm_struct *parent,
				  struct mm_struct *child);
static bool fork_pmd_share_supported =
	IS_ENABLED(CONFIG_HUGETLB_PMD_PAGE_TABLE_SHARING);
module_param(fork_pmd_share_supported, bool, 0444);
static bool fork_pmd_diagnostics;
module_param(fork_pmd_diagnostics, bool, 0444);
static atomic_long_t fork_pmd_shared = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_pmd_absent = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_pmd_unshared = ATOMIC_LONG_INIT(0);

static bool fork_mm_cache = true;
module_param(fork_mm_cache, bool, 0444);
MODULE_PARM_DESC(fork_mm_cache, "Reuse compatible inactive fork execution MMs");
static bool fork_mm_reservations = true;
module_param(fork_mm_reservations, bool, 0444);
MODULE_PARM_DESC(fork_mm_reservations,
		 "Preserve matching empty alias reservations");
static bool fork_mm_kernel_reservations = true;
module_param(fork_mm_kernel_reservations, bool, 0444);
MODULE_PARM_DESC(fork_mm_kernel_reservations,
		 "Preserve empty reservations in the shared kernel-vmalloc window");
/* Read-only diagnostics; the cache never depends on these counters. */
static atomic_long_t fork_mm_budget_rejects = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_hits = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_misses = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_saved = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_cached = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_bypasses = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_reservations_kept = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_reservations_pruned = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_kernel_reservations_kept = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_prune_user_rejects = ATOMIC_LONG_INIT(0);
static atomic_long_t fork_mm_prune_kernel_rejects = ATOMIC_LONG_INIT(0);

static int flux_fork_counter_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%ld\n", atomic_long_read(kp->arg));
}

static const struct kernel_param_ops flux_fork_counter_ops = {
	.get = flux_fork_counter_get,
};
module_param_cb(fork_pmd_shared, &flux_fork_counter_ops, &fork_pmd_shared,
		0444);
module_param_cb(fork_pmd_absent, &flux_fork_counter_ops, &fork_pmd_absent,
		0444);
module_param_cb(fork_pmd_unshared, &flux_fork_counter_ops, &fork_pmd_unshared,
		0444);
module_param_cb(fork_mm_budget_rejects, &flux_fork_counter_ops,
		&fork_mm_budget_rejects, 0444);
module_param_cb(fork_mm_hits, &flux_fork_counter_ops, &fork_mm_hits, 0444);
module_param_cb(fork_mm_misses, &flux_fork_counter_ops, &fork_mm_misses, 0444);
module_param_cb(fork_mm_saved, &flux_fork_counter_ops, &fork_mm_saved, 0444);
module_param_cb(fork_mm_cached, &flux_fork_counter_ops, &fork_mm_cached, 0444);
module_param_cb(fork_mm_bypasses, &flux_fork_counter_ops, &fork_mm_bypasses,
		0444);
module_param_cb(fork_mm_reservations_kept, &flux_fork_counter_ops,
		&fork_mm_reservations_kept, 0444);
module_param_cb(fork_mm_reservations_pruned, &flux_fork_counter_ops,
		&fork_mm_reservations_pruned, 0444);
module_param_cb(fork_mm_kernel_reservations_kept, &flux_fork_counter_ops,
		&fork_mm_kernel_reservations_kept, 0444);
module_param_cb(fork_mm_prune_user_rejects, &flux_fork_counter_ops,
		&fork_mm_prune_user_rejects, 0444);
module_param_cb(fork_mm_prune_kernel_rejects, &flux_fork_counter_ops,
		&fork_mm_prune_kernel_rejects, 0444);

struct flux_fork_mm_entry {
	struct mm_struct *mm;
	struct mm_struct *parent;
	/* Resource charges captured after retirement and reservation pruning. */
	unsigned long pgtables, private_pages, reservation_pages;
	unsigned int vmas;
};

static bool flux_fork_mm_reject(enum flux_fork_stat reason)
{
	flux_fork_stats_note(reason);
	return false;
}

/* Called under the cache lock, before publishing or displacing any entry. */
static bool flux_fork_mm_budget(struct flux_mm_ctx *ctx,
				const struct flux_fork_mm_entry *entry,
				unsigned int slot)
{
	unsigned long pgtables = entry->pgtables;
	unsigned long private_pages = entry->private_pages;
	unsigned long reservation_pages = entry->reservation_pages;
	unsigned int vmas = entry->vmas, i;

	lockdep_assert_held(&ctx->fork_cache_lock);
	for (i = 0; i < FLUX_FORK_MM_CACHE_SIZE; i++) {
		const struct flux_fork_mm_entry *old = ctx->fork_cache[i];

		if (!old || i == slot)
			continue;
		pgtables += old->pgtables;
		private_pages += old->private_pages;
		reservation_pages += old->reservation_pages;
		vmas += old->vmas;
	}
	return pgtables <= FLUX_FORK_CACHE_PGTABLE_BYTES &&
	       private_pages <= FLUX_FORK_CACHE_PRIVATE_PAGES &&
	       reservation_pages <= FLUX_FORK_CACHE_RESERVATION_PAGES &&
	       vmas <= FLUX_FORK_CACHE_VMAS;
}

/* The MM and VMA write locks exclude faults and PTE-table removal. */
static bool flux_fork_pte_slot(struct mm_struct *mm, unsigned long addr,
			       pte_t **slot, spinlock_t **lock)
{
	pgd_t *pgd = pgd_offset(mm, addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	*slot = NULL;
	*lock = NULL;
	if (pgd_none(*pgd))
		return true;
	if (pgd_bad(*pgd))
		return false;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return true;
	if (p4d_bad(*p4d))
		return false;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return true;
	if (pud_leaf(*pud) || pud_bad(*pud))
		return false;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return true;
	if (pmd_leaf(*pmd) || pmd_bad(*pmd))
		return false;
	/* Flux's x86 host has no highmem PTE pages. */
	*slot = pte_offset_kernel(pmd, addr);
	*lock = pte_lockptr(mm, pmd);
	return true;
}

/* The process is unreachable, its alias index and source pins retired. */
static bool flux_fork_reservation_empty(struct vm_area_struct *vma)
{
	unsigned long addr;

	for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
		spinlock_t *lock;
		pte_t *slot;
		bool empty;

		if (!flux_fork_pte_slot(vma->vm_mm, addr, &slot, &lock))
			return false;
		if (!slot)
			continue;
		spin_lock(lock);
		empty = pte_none(ptep_get(slot));
		spin_unlock(lock);
		if (!empty)
			return false;
	}
	return true;
}

static bool flux_fork_pte_equal(struct vm_area_struct *a,
				struct vm_area_struct *b, unsigned long addr)
{
	pte_t *xp, *yp, x, y;
	spinlock_t *xl, *yl;
	bool equal = false;

	if (!flux_fork_pte_slot(a->vm_mm, addr, &xp, &xl) ||
	    !flux_fork_pte_slot(b->vm_mm, addr, &yp, &yl))
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_PTE_WALK);
	/* No other two-MM operation can involve the inactive MM. */
	if (xl)
		spin_lock(xl);
	if (yl && yl != xl)
		spin_lock_nested(yl, SINGLE_DEPTH_NESTING);
	x = xp ? ptep_get(xp) : __pte(0);
	y = yp ? ptep_get(yp) : __pte(0);
	if (pte_none(x) && pte_none(y)) {
		equal = true;
		goto out;
	}
	if (!pte_present(x) || !pte_present(y)) {
		flux_fork_stats_note(FLUX_FORK_STAT_REJECT_PTE_PRESENT);
		goto out;
	}
	if (pte_write(x) || pte_write(y)) {
		flux_fork_stats_note(FLUX_FORK_STAT_REJECT_PTE_WRITE);
		goto out;
	}
	if (pte_pfn(x) != pte_pfn(y)) {
		flux_fork_stats_note(FLUX_FORK_STAT_REJECT_PTE_PFN);
		goto out;
	}
	if (pgprot_val(pte_pgprot(pte_mkold(pte_mkclean(x)))) !=
	    pgprot_val(pte_pgprot(pte_mkold(pte_mkclean(y))))) {
		flux_fork_stats_note(FLUX_FORK_STAT_REJECT_PTE_PROT);
		goto out;
	}
	if (a->anon_vma && (pte_special(x) || pte_special(y) ||
			    folio_maybe_dma_pinned(page_folio(pte_page(x))))) {
		flux_fork_stats_note(FLUX_FORK_STAT_REJECT_PTE_SPECIAL);
		goto out;
	}
	equal = true;
out:
	if (yl && yl != xl)
		spin_unlock(yl);
	if (xl)
		spin_unlock(xl);
	return equal;
}

/* Accept page-cache mappings and Flux's explicitly coherent backing objects. */
static bool flux_fork_file_mapping(const struct vm_area_struct *vma)
{
	return vma->vm_file && vma->vm_ops &&
	       !(vma->vm_flags & (VM_IO | VM_PFNMAP | VM_MIXEDMAP)) &&
	       (shmem_file(vma->vm_file) ||
		vma->vm_ops->map_pages == filemap_map_pages);
}

static bool flux_fork_private_equal(struct vm_area_struct *a,
				    struct vm_area_struct *b)
{
	unsigned long addr;

	/* Clean private file mappings follow the same page cache. */
	if (flux_fork_file_mapping(a) && !a->anon_vma && !b->anon_vma)
		return true;
	/* Unfaulted vDSO pages still refer to the same immutable host image. */
	if (flux_vdso_mapping && a->vm_private_data == flux_vdso_mapping &&
	    !a->anon_vma && !b->anon_vma && !(a->vm_flags & VM_WRITE) &&
	    a->vm_mm->context.vdso == b->vm_mm->context.vdso &&
	    a->vm_mm->context.vdso_image == b->vm_mm->context.vdso_image)
		return true;
	/* Unknown special/device fault handlers may carry per-MM state. */
	if (!vma_is_anonymous(a) && !flux_fork_file_mapping(a) &&
	    !(flux_vdso_mapping && a->vm_private_data == flux_vdso_mapping) &&
	    !(flux_vvar_mapping && a->vm_private_data == flux_vvar_mapping) &&
	    !(flux_vclock_mapping && a->vm_private_data == flux_vclock_mapping))
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_PRIVATE_HANDLER);
	if (!!a->anon_vma != !!b->anon_vma ||
	    (a->anon_vma && a->anon_vma->root != b->anon_vma->root))
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_ANON_ROOT);
	/* Bound the private-PTE validation work as well as the MM count. */
	if (a->vm_end - a->vm_start > 64 * PAGE_SIZE)
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_PRIVATE_SIZE);
	for (addr = a->vm_start; addr < a->vm_end; addr += PAGE_SIZE)
		if (!flux_fork_pte_equal(a, b, addr))
			return false;
	return true;
}

static bool flux_fork_vma_equal(struct vm_area_struct *a,
				struct vm_area_struct *b)
{
	/* dup_mmap clears locking flags; other attributes must match. */
	vm_flags_t flags = a->vm_flags & ~(VM_LOCKED | VM_LOCKONFAULT);

	if (a->vm_start != b->vm_start || a->vm_end != b->vm_end)
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_RANGE);
	if (flags != b->vm_flags)
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_FLAGS);
	if (a->vm_file != b->vm_file || a->vm_pgoff != b->vm_pgoff)
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_FILE);
	if (a->vm_ops != b->vm_ops)
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_OPS);
	if (a->vm_private_data != b->vm_private_data && !is_vm_hugetlb_page(a))
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_PRIVATE);
	if (pgprot_val(a->vm_page_prot) != pgprot_val(b->vm_page_prot))
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_PROT);
	if (userfaultfd_armed(a) || userfaultfd_armed(b) || (flags & VM_WIPEONFORK))
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_SPECIAL);
#ifdef CONFIG_NUMA
	if (a->vm_policy || b->vm_policy)
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_VMA_POLICY);
#endif
	if (is_vm_hugetlb_page(a)) {
		if ((flags & VM_SHARED) && a->vm_file &&
		    a->vm_start >= FLUX_PHYS_BASE && !a->anon_vma &&
		    !b->anon_vma && huge_page_size(hstate_vma(a)) == SZ_2M)
			return true;
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_HUGETLB);
	}
	if (flags & VM_SHARED) {
		if (flux_fork_file_mapping(a) || flux_control_vma_shared(a) ||
		    flux_backing_vma_shared(a))
			return true;
		return flux_fork_mm_reject(FLUX_FORK_STAT_REJECT_SHARED);
	}
	return flux_fork_private_equal(a, b);
}

/* Preserve the cached MM's identity/TLB state; compare inherited ABI state. */
static bool flux_fork_layout_equal(struct mm_struct *a, struct mm_struct *b)
{
	if (a->task_size != b->task_size || a->mmap_base != b->mmap_base ||
	    a->mmap_legacy_base != b->mmap_legacy_base ||
	    !flux_compat_mm_area_equal(a, b) || a->def_flags != b->def_flags ||
	    (a->flags & MMF_INIT_MASK) != (b->flags & MMF_INIT_MASK) ||
	    a->exe_file != b->exe_file || a->binfmt != b->binfmt ||
	    a->user_ns != b->user_ns || a->start_code != b->start_code ||
	    a->end_code != b->end_code || a->start_data != b->start_data ||
	    a->end_data != b->end_data || a->start_brk != b->start_brk ||
	    a->brk != b->brk || a->start_stack != b->start_stack ||
	    a->arg_start != b->arg_start || a->arg_end != b->arg_end ||
	    a->env_start != b->env_start || a->env_end != b->env_end ||
	    memcmp(a->saved_auxv, b->saved_auxv, sizeof(a->saved_auxv)) ||
	    a->context.vdso != b->context.vdso ||
	    a->context.vdso_image != b->context.vdso_image ||
	    a->context.flags != b->context.flags ||
	    atomic_read(&a->context.perf_rdpmc_allowed) ||
	    atomic_read(&b->context.perf_rdpmc_allowed))
		return false;
#ifdef CONFIG_MODIFY_LDT_SYSCALL
	if (a->context.ldt || b->context.ldt)
		return false;
#endif
#ifdef CONFIG_ADDRESS_MASKING
	if (a->context.lam_cr3_mask || b->context.lam_cr3_mask ||
	    a->context.untag_mask != b->context.untag_mask)
		return false;
#endif
#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
	if (a->context.pkey_allocation_map != b->context.pkey_allocation_map ||
	    a->context.execute_only_pkey != b->context.execute_only_pkey)
		return false;
#endif
	return true;
}

/* Both MMs are write locked; only stateless empty reservations are excluded. */
static bool flux_fork_reservation_matches(struct vm_area_struct *source,
					  struct vm_area_struct *target)
{
	return source && source->vm_start <= target->vm_start &&
	       source->vm_end >= target->vm_end &&
	       flux_alias_reservation_vma(source) &&
	       source->vm_flags == target->vm_flags &&
	       source->vm_ops == target->vm_ops &&
	       pgprot_val(source->vm_page_prot) ==
		       pgprot_val(target->vm_page_prot);
}

/*
 * Kernel-vmalloc addresses belong to the context's shared init_mm, not an
 * application's private layout. A retired reservation there is only an empty
 * container: reuse does not preserve a kernel PTE or a source-page pin. The
 * normal checked fault path reconstructs it from the current global Flux PTE.
 *
 * Both host MMs are write locked. Keep only a fully contained, bounded, empty
 * reservation of the known stateless type. A parent runtime mapping or a
 * differently typed/protected reservation is a conflict, never an exemption.
 */
static bool
flux_fork_kernel_reservation_matches(struct mm_struct *parent,
				     struct vm_area_struct *target)
{
	struct vm_area_struct *source;
	unsigned long start = target->vm_start, end = target->vm_end;

	if (!fork_mm_kernel_reservations || start < FLUX_VMALLOC_START ||
	    end > FLUX_VMALLOC_END ||
	    (target->vm_ops && target->vm_ops != flux_vma_dummy_vm_ops) ||
	    (target->vm_flags & VM_SHARED) || vma_pkey(target) ||
	    vma_pages(target) > FLUX_FORK_RESERVATION_PAGES ||
	    !flux_alias_reservation_vma(target) ||
	    !flux_fork_reservation_empty(target))
		return false;
	for (source = find_vma(parent, start); source && source->vm_start < end;
	     source = find_vma(parent, source->vm_end)) {
		if (!flux_alias_reservation_vma(source) ||
		    source->vm_flags != target->vm_flags ||
		    source->vm_ops != target->vm_ops ||
		    pgprot_val(source->vm_page_prot) !=
			    pgprot_val(target->vm_page_prot))
			return false;
	}
	return true;
}

static bool flux_fork_mm_equal(struct mm_struct *parent, struct mm_struct *mm)
{
	VMA_ITERATOR(src, parent, 0);
	VMA_ITERATOR(dst, mm, 0);
	struct vm_area_struct *a, *b;
	unsigned int kept = 0, kernel_kept = 0, pruned = 0;
	bool equal = false;

	mmap_write_lock(parent);
	/* The cached MM is unreachable and has no task or gate references. */
	mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING);
	if (!flux_fork_layout_equal(parent, mm)) {
		flux_fork_stats_note(FLUX_FORK_STAT_REJECT_LAYOUT);
		goto out;
	}
	a = vma_next(&src);
	b = vma_next(&dst);
	while (a || b) {
		if (b && (b->vm_flags & VM_DONTCOPY) &&
		    flux_alias_reservation_vma(b)) {
			unsigned long start = b->vm_start, end = b->vm_end;

			/* Advance only excluded source VMAs; an inherited VMA
			 * before b must still have a matching target counterpart.
			 */
			while (a && (a->vm_flags & VM_DONTCOPY) &&
			       a->vm_end <= start)
				a = vma_next(&src);
			if (flux_fork_reservation_matches(a, b)) {
				kept++;
			} else if (flux_fork_kernel_reservation_matches(parent, b)) {
				kept++;
				kernel_kept++;
			} else {
				/* A child-only address or changed mapping kind must
				 * not survive into the next execution. Removing an
				 * empty VMA cannot change any inherited runtime data.
				 */
				if (flux_compat_prune_vma(flux_do_munmap, mm,
							  start, end - start)) {
					atomic_long_inc(flux_kernel_alias(start) ?
						&fork_mm_prune_kernel_rejects :
						&fork_mm_prune_user_rejects);
					flux_fork_stats_note(FLUX_FORK_STAT_REJECT_PRUNE);
					goto out;
				}
				pruned++;
				vma_iter_init(&dst, mm, end);
			}
			b = vma_next(&dst);
			continue;
		}
		if (a && (a->vm_flags & VM_DONTCOPY)) {
			a = vma_next(&src);
			continue;
		}
		if (!a || !b) {
			flux_fork_stats_note(FLUX_FORK_STAT_REJECT_VMA_COUNT);
			goto out;
		}
		vma_start_write(a);
		if (!flux_fork_vma_equal(a, b))
			goto out;
		a = vma_next(&src);
		b = vma_next(&dst);
	}
	equal = true;
out:
	if (kept)
		atomic_long_add(kept, &fork_mm_reservations_kept);
	if (kernel_kept)
		atomic_long_add(kernel_kept, &fork_mm_kernel_reservations_kept);
	if (pruned)
		atomic_long_add(pruned, &fork_mm_reservations_pruned);
	mmap_write_unlock(mm);
	mmap_write_unlock(parent);
	return equal;
}

/* The cache lock protects retry state; only a construction consumes a retry. */
static bool flux_fork_skip_locked(struct flux_mm_ctx *ctx,
				  struct mm_struct *parent, bool consume)
{
	unsigned int i;

	for (i = 0; i < FLUX_FORK_MM_RETRY_SIZE; i++) {
		struct flux_fork_mm_retry *retry = &ctx->fork_retry[i];

		if (retry->parent == parent && retry->remaining) {
			if (consume)
				retry->remaining--;
			return true;
		}
	}
	return false;
}

static void flux_fork_retry_later(struct flux_mm_ctx *ctx,
				  struct mm_struct *parent)
{
	struct mm_struct *old;
	unsigned int i, slot;

	mmgrab(parent);
	spin_lock(&ctx->fork_cache_lock);
	slot = ctx->fork_retry_next;
	for (i = 0; i < FLUX_FORK_MM_RETRY_SIZE; i++) {
		if (ctx->fork_retry[i].parent == parent) {
			slot = i;
			break;
		}
	}
	old = ctx->fork_retry[slot].parent;
	ctx->fork_retry[slot].parent = parent;
	ctx->fork_retry[slot].remaining = FLUX_FORK_MM_RETRY_INTERVAL;
	ctx->fork_retry_next = (slot + 1) % FLUX_FORK_MM_RETRY_SIZE;
	spin_unlock(&ctx->fork_cache_lock);
	if (old)
		mmdrop(old);
}

/**
 * flux_fork_mm_get() - consume a compatible inactive MM, or return NULL
 * @ctx: owner of the bounded cache
 * @parent: referenced fork parent, with its alias gate already held
 *
 * A successful return transfers one mm_users reference to the caller. Neither
 * VMA comparisons nor MM destruction run while the cache spinlock is held.
 */
struct mm_struct *flux_fork_mm_get(struct flux_mm_ctx *ctx,
				   struct mm_struct *parent)
{
	struct flux_fork_mm_entry *entry = NULL;
	struct mm_struct *mm;
	int i;

	if (!fork_mm_cache) {
		flux_fork_stats_note(FLUX_FORK_STAT_GET_DISABLED);
		return NULL;
	}
	if (ctx->mpk_enabled || !flux_do_munmap) {
		flux_fork_stats_note(FLUX_FORK_STAT_GET_INELIGIBLE);
		return NULL;
	}
	spin_lock(&ctx->fork_cache_lock);
	if (flux_fork_skip_locked(ctx, parent, true)) {
		spin_unlock(&ctx->fork_cache_lock);
		atomic_long_inc(&fork_mm_bypasses);
		flux_fork_stats_note(FLUX_FORK_STAT_GET_BACKOFF);
		return NULL;
	}
	for (i = 0; i < FLUX_FORK_MM_CACHE_SIZE; i++) {
		if (ctx->fork_cache[i] &&
		    ctx->fork_cache[i]->parent == parent) {
			entry = ctx->fork_cache[i];
			ctx->fork_cache[i] = NULL;
			atomic_long_dec(&fork_mm_cached);
			break;
		}
	}
	spin_unlock(&ctx->fork_cache_lock);
	if (!entry) {
		flux_fork_stats_note(FLUX_FORK_STAT_GET_EMPTY);
		return NULL;
	}
	mm = entry->mm;
	mmdrop(entry->parent);
	kfree(entry);
	if (flux_fork_mm_equal(parent, mm)) {
		atomic_long_inc(&fork_mm_hits);
		flux_fork_stats_note(FLUX_FORK_STAT_GET_HIT);
		return mm;
	}
	atomic_long_inc(&fork_mm_misses);
	flux_fork_stats_note(FLUX_FORK_STAT_GET_REJECT);
	flux_fork_retry_later(ctx, parent);
	mmput(mm);
	return NULL;
}

/**
 * flux_fork_mm_put() - transfer a retired process MM into the cache
 * @ctx: owner of the bounded cache
 * @proc: unreachable process slot, with all alias PTEs and pins retired
 *
 * Return: true if the cache consumed the MM and parent references. A false
 * return leaves them with the caller. Any replacement is destroyed unlocked.
 */
bool flux_fork_mm_put(struct flux_mm_ctx *ctx, struct flux_proc *proc)
{
	struct mm_struct *mm = proc->mm;
	struct flux_fork_mm_entry *entry, *victim = NULL;
	struct vm_area_struct *vma;
	unsigned long addr = 0, private_pages = 0, reservation_pages = 0;
	int i, ret = 0;
	bool saved = false, skip;

	/* Shared file page tables are populated lazily after fork. Observe the
	 * retired child, after it has actually accessed its runtime mappings.
	 */
	if (fork_pmd_diagnostics && proc->fork_parent &&
	    atomic_read(&mm->mm_users) == 1 &&
	    mmget_not_zero(proc->fork_parent)) {
		flux_fork_pmd_observe(proc->fork_parent, mm);
		mmput(proc->fork_parent);
	}

	if (!fork_mm_cache)
		return flux_fork_mm_reject(FLUX_FORK_STAT_PUT_DISABLED);
	if (ctx->mpk_enabled || !flux_do_munmap ||
	    !proc->fork_parent || READ_ONCE(ctx->fork_cache_stopping) ||
	    atomic_read(&mm->mm_users) != 1 || mm->map_count > 256 ||
	    mm_pgtables_bytes(mm) > SZ_2M)
		return flux_fork_mm_reject(FLUX_FORK_STAT_PUT_INELIGIBLE);
	spin_lock(&ctx->fork_cache_lock);
	skip = flux_fork_skip_locked(ctx, proc->fork_parent, false);
	spin_unlock(&ctx->fork_cache_lock);
	if (skip)
		return flux_fork_mm_reject(FLUX_FORK_STAT_PUT_BACKOFF);
	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return flux_fork_mm_reject(FLUX_FORK_STAT_PUT_ALLOC);
	mmap_write_lock(mm);
	while ((vma = find_vma(mm, addr))) {
		unsigned long start = vma->vm_start, end = vma->vm_end;

		addr = end;
		if (!(vma->vm_flags & VM_SHARED) && vma->anon_vma) {
			private_pages += vma_pages(vma);
			if (private_pages > 64) {
				flux_fork_stats_note(FLUX_FORK_STAT_PUT_PRIVATE_SIZE);
				ret = -E2BIG;
				break;
			}
		}
		if (vma->vm_flags & VM_DONTCOPY) {
			/* Keep only a bounded set of proven-empty Flux reservations. */
			if (fork_mm_reservations &&
			    flux_alias_reservation_vma(vma) &&
			    vma_pages(vma) <= FLUX_FORK_RESERVATION_PAGES -
						      reservation_pages &&
			    flux_fork_reservation_empty(vma)) {
				reservation_pages += vma_pages(vma);
				continue;
			}
			ret = flux_compat_prune_vma(flux_do_munmap, mm, start,
						    end - start);
			if (ret) {
				flux_fork_stats_note(FLUX_FORK_STAT_PUT_PRUNE);
				break;
			}
		}
	}
	entry->pgtables = mm_pgtables_bytes(mm);
	entry->vmas = mm->map_count;
	entry->private_pages = private_pages;
	entry->reservation_pages = reservation_pages;
	mmap_write_unlock(mm);
	if (ret)
		goto out;
	entry->mm = mm;
	entry->parent = proc->fork_parent;
	spin_lock(&ctx->fork_cache_lock);
	if (!ctx->fork_cache_stopping) {
		unsigned int slot = ctx->fork_cache_next;

		/* Empty or dead-parent entries should not evict a useful MM. */
		for (i = 0; i < FLUX_FORK_MM_CACHE_SIZE; i++) {
			unsigned int index =
				(slot + i) % FLUX_FORK_MM_CACHE_SIZE;
			struct flux_fork_mm_entry *old = ctx->fork_cache[index];

			if (!old || !atomic_read(&old->parent->mm_users)) {
				slot = index;
				break;
			}
		}
		if (!flux_fork_mm_budget(ctx, entry, slot)) {
			atomic_long_inc(&fork_mm_budget_rejects);
			flux_fork_stats_note(FLUX_FORK_STAT_PUT_BUDGET);
			goto unlock;
		}
		victim = ctx->fork_cache[slot];
		if (!victim)
			atomic_long_inc(&fork_mm_cached);
		ctx->fork_cache[slot] = entry;
		ctx->fork_cache_next = (slot + 1) % FLUX_FORK_MM_CACHE_SIZE;
		proc->fork_parent = NULL;
		saved = true;
	} else {
		flux_fork_stats_note(FLUX_FORK_STAT_PUT_STOPPING);
	}
unlock:
	spin_unlock(&ctx->fork_cache_lock);
	if (victim) {
		mmput(victim->mm);
		mmdrop(victim->parent);
		kfree(victim);
	}
out:
	if (!saved)
		kfree(entry);
	else {
		atomic_long_inc(&fork_mm_saved);
		flux_fork_stats_note(FLUX_FORK_STAT_PUT_SAVED);
	}
	return saved;
}

/**
 * flux_fork_mm_drain() - permanently stop insertion and release cached MMs
 * @ctx: context entering its final release
 */
void flux_fork_mm_drain(struct flux_mm_ctx *ctx)
{
	struct flux_fork_mm_entry *entries[FLUX_FORK_MM_CACHE_SIZE];
	struct mm_struct *parents[FLUX_FORK_MM_RETRY_SIZE];
	int i;

	spin_lock(&ctx->fork_cache_lock);
	ctx->fork_cache_stopping = true;
	for (i = 0; i < FLUX_FORK_MM_RETRY_SIZE; i++) {
		parents[i] = ctx->fork_retry[i].parent;
		ctx->fork_retry[i].parent = NULL;
	}
	for (i = 0; i < FLUX_FORK_MM_CACHE_SIZE; i++) {
		entries[i] = ctx->fork_cache[i];
		if (entries[i])
			atomic_long_dec(&fork_mm_cached);
		ctx->fork_cache[i] = NULL;
	}
	spin_unlock(&ctx->fork_cache_lock);
	for (i = 0; i < FLUX_FORK_MM_RETRY_SIZE; i++) {
		if (parents[i])
			mmdrop(parents[i]);
	}
	for (i = 0; i < FLUX_FORK_MM_CACHE_SIZE; i++) {
		if (!entries[i])
			continue;
		mmput(entries[i]->mm);
		mmdrop(entries[i]->parent);
		kfree(entries[i]);
	}
}

/* Only compare PUD values, never export or dereference a borrowed PMD page. */
static unsigned long flux_fork_pmd_page(struct mm_struct *mm,
					unsigned long addr)
{
	pgd_t *pgd = pgd_offset(mm, addr);
	p4d_t *p4d;
	pud_t *pud;
	pud_t value;

	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;
	pud = pud_offset(p4d, addr);
	value = READ_ONCE(*pud);
	if (!pud_present(value) || pud_leaf(value))
		return 0;
	return pud_pfn(value);
}

static void flux_fork_pmd_observe(struct mm_struct *parent,
				  struct mm_struct *child)
{
	struct vm_area_struct *vma;
	unsigned long addr = FLUX_PHYS_BASE;

	if (!fork_pmd_diagnostics)
		return;
	/* Child is retired; the caller holds a live reference on its parent. */
	mmap_write_lock(parent);
	mmap_write_lock_nested(child, SINGLE_DEPTH_NESTING);
	while ((vma = find_vma(parent, addr))) {
		unsigned long end = vma->vm_end;

		if (is_vm_hugetlb_page(vma) && (vma->vm_flags & VM_SHARED)) {
			for (addr = ALIGN(vma->vm_start, SZ_1G);
			     addr < end && end - addr >= SZ_1G; addr += SZ_1G) {
				unsigned long a =
					flux_fork_pmd_page(parent, addr);
				unsigned long b =
					flux_fork_pmd_page(child, addr);

				if (!a || !b)
					atomic_long_inc(&fork_pmd_absent);
				else
					atomic_long_inc(
						a == b ? &fork_pmd_shared :
							 &fork_pmd_unshared);
			}
		}
		addr = end;
	}
	mmap_write_unlock(child);
	mmap_write_unlock(parent);
}
