// SPDX-License-Identifier: GPL-2.0
/* Demand projection of private-anonymous fork pages, with bounded read-ahead. */
#include <linux/delay.h>
#include <linux/maple_tree.h>

#include <flux/host_abi.h>

#include "alias-internal.h"

#define FLUX_PROJECTION_BATCH_MAX 64
#define FLUX_PROJECTION_INVALIDATION_LIMIT 1024

static bool fork_fault_repair;
module_param(fork_fault_repair, bool, 0444);
MODULE_PARM_DESC(fork_fault_repair,
		 "Resolve admitted fork-child read faults before SIGSEGV");

static unsigned int fork_fault_batch = 32;
module_param(fork_fault_batch, uint, 0444);
MODULE_PARM_DESC(fork_fault_batch,
		 "Maximum pages projected per read fault (1-64)");

/* Prepared/pending/retired count admitted virtual pages, not allocated pins. */
static atomic_long_t fault_prepared = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_repaired = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_retired = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_pending = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_failed = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_pages = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_fallback = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_no_domain = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_no_range = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_bad_walk = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_no_pin = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_no_reservation = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_full_flush = ATOMIC_LONG_INIT(0);
static atomic_t fault_hook_calls = ATOMIC_INIT(0);

static int fault_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%ld", atomic_long_read(kp->arg));
}

static const struct kernel_param_ops fault_count_ops = {
	.get = fault_count_get,
};
module_param_cb(fork_fault_prepared, &fault_count_ops, &fault_prepared, 0444);
module_param_cb(fork_fault_repaired, &fault_count_ops, &fault_repaired, 0444);
module_param_cb(fork_fault_retired, &fault_count_ops, &fault_retired, 0444);
module_param_cb(fork_fault_pending, &fault_count_ops, &fault_pending, 0444);
module_param_cb(fork_fault_failed, &fault_count_ops, &fault_failed, 0444);
module_param_cb(fork_fault_pages, &fault_count_ops, &fault_pages, 0444);
module_param_cb(fork_fault_fallback, &fault_count_ops, &fault_fallback, 0444);
module_param_cb(fork_fault_no_domain, &fault_count_ops, &fault_no_domain, 0444);
module_param_cb(fork_fault_no_range, &fault_count_ops, &fault_no_range, 0444);
module_param_cb(fork_fault_bad_walk, &fault_count_ops, &fault_bad_walk, 0444);
module_param_cb(fork_fault_no_pin, &fault_count_ops, &fault_no_pin, 0444);
module_param_cb(fork_fault_no_reservation, &fault_count_ops,
		&fault_no_reservation, 0444);
module_param_cb(fork_fault_full_flush, &fault_count_ops, &fault_full_flush,
		0444);

struct flux_projection {
	unsigned long pgd;
	unsigned long memory_start;
	unsigned long memory_end;
	unsigned long admitted_pages;
	unsigned long next_read;
	unsigned int read_window;
	unsigned int invalidations;
	struct maple_tree admission;
	/* The alias mutex serializes this scratch space; keep it off fault stacks. */
	u64 leaves[FLUX_PROJECTION_BATCH_MAX];
	struct page *pages[FLUX_PROJECTION_BATCH_MAX];
	struct flux_projection_range ranges[];
};

/*
 * The alias mutex is also a software page-table walker grace period. Guest
 * TLB completion enters this lock even when no hardware aliases exist. It
 * retires overlapping admission before guest pages or tables can be reused.
 * arch_unmap supplies the same barrier for VMAs with no present PTEs.
 */
void flux_projection_invalidate_locked(struct flux_alias_mm *alias_mm,
				       unsigned long start, unsigned long end)
{
	struct flux_projection *projection = alias_mm->projection;
	unsigned long pages = 0;
	MA_STATE(mas, NULL, start, end - 1);

	lockdep_assert_held(flux_alias_mm_lock(alias_mm->mm));
	if (!projection || !projection->admitted_pages || start >= end)
		return;
	if ((!start && end == ULONG_MAX) ||
	    projection->invalidations == FLUX_PROJECTION_INVALIDATION_LIMIT) {
		if (!start && end == ULONG_MAX)
			atomic_long_inc(&fault_full_flush);
		pages = projection->admitted_pages;
		mtree_destroy(&projection->admission);
		mt_init(&projection->admission);
	} else {
		mas.tree = &projection->admission;
		rcu_read_lock();
		while (mas_find(&mas, end - 1)) {
			unsigned long first = max(start, mas.index);
			unsigned long last = min(end, mas.last + 1);

			pages += (last - first) >> PAGE_SHIFT;
		}
		rcu_read_unlock();
		if (!pages)
			return;
		projection->invalidations++;
		if (mtree_store_range(&projection->admission, start, end - 1,
				      NULL, GFP_KERNEL)) {
			/* Failure may lose acceleration, never retain stale admission. */
			pages = projection->admitted_pages;
			mtree_destroy(&projection->admission);
			mt_init(&projection->admission);
		}
	}
	projection->admitted_pages -= pages;
	atomic_long_sub(pages, &fault_pending);
	atomic_long_add(pages, &fault_retired);
}

void flux_projection_mm_release(struct flux_alias_mm *alias_mm)
{
	lockdep_assert_held(flux_alias_mm_lock(alias_mm->mm));
	flux_projection_invalidate_locked(alias_mm, 0, ULONG_MAX);
	if (alias_mm->projection)
		mtree_destroy(&alias_mm->projection->admission);
	kvfree(alias_mm->projection);
	alias_mm->projection = NULL;
}

int flux_bind_fork_projection(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_bind_fork_projection a;
	struct flux_projection *projection;
	struct flux_alias_mm *alias_mm;
	struct flux_proc *proc;
	unsigned long pages = 0;
	unsigned int i;
	int ret = -EINVAL;

	if (!fork_fault_repair || READ_ONCE(ctx->mpk_enabled))
		return 0;
	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	if (!a.proc_key || !a.nr_ranges ||
	    a.nr_ranges > FLUX_PROJECTION_MAX_RANGES || !a.ranges ||
	    a.memory_start < FLUX_PHYS_BASE || a.memory_end > TASK_SIZE_MAX ||
	    a.memory_end <= a.memory_start ||
	    a.memory_end - a.memory_start < PAGE_SIZE ||
	    ((a.pgd | a.memory_start | a.memory_end) & ~PAGE_MASK) ||
	    a.pgd < a.memory_start || a.pgd > a.memory_end - PAGE_SIZE)
		return -EINVAL;
	projection = kvzalloc(struct_size(projection, ranges, a.nr_ranges),
			      GFP_KERNEL);
	if (!projection)
		return -ENOMEM;
	mt_init(&projection->admission);
	if (copy_from_user(projection->ranges, (void __user *)a.ranges,
			   a.nr_ranges * sizeof(*projection->ranges))) {
		ret = -EFAULT;
		goto out_free;
	}
	for (i = 0; i < a.nr_ranges; i++) {
		struct flux_projection_range *range = &projection->ranges[i];

		if (range->start >= range->end || range->end > FLUX_PHYS_BASE ||
		    ((range->start | range->end) & ~PAGE_MASK) ||
		    (range->start < FLUX_VMALLOC_END &&
		     range->end > FLUX_VMALLOC_START) ||
		    (i && projection->ranges[i - 1].end > range->start)) {
			ret = -EINVAL;
			goto out_free;
		}
		ret = mtree_store_range(&projection->admission, range->start,
					range->end - 1, range, GFP_KERNEL);
		if (ret)
			goto out_free;
		pages += (range->end - range->start) >> PAGE_SHIFT;
	}
	projection->pgd = a.pgd;
	projection->memory_start = a.memory_start;
	projection->memory_end = a.memory_end;
	projection->admitted_pages = pages;
	ret = -EINVAL;
	proc = flux_proc_get_by_key(ctx, a.proc_key);
	if (!proc) {
		ret = -ENOENT;
		goto out_free;
	}
	if (!proc->for_fork || !proc->mm || proc->mm == current->mm ||
	    proc->fork_parent != current->mm ||
	    atomic_read(&proc->alias_state) != FLUX_ALIAS_STATE_ACTIVE)
		goto out_proc;
	mutex_lock(flux_alias_mm_lock(proc->mm));
	alias_mm = flux_alias_mm_get_locked(ctx, proc->mm, GFP_KERNEL);
	if (!alias_mm) {
		ret = -ENOMEM;
	} else if (!alias_mm->released && !alias_mm->projection &&
		   !READ_ONCE(proc->released)) {
		alias_mm->projection = projection;
		projection = NULL;
		atomic_long_add(pages, &fault_prepared);
		atomic_long_add(pages, &fault_pending);
		ret = 0;
	}
	mutex_unlock(flux_alias_mm_lock(proc->mm));
out_proc:
	flux_proc_put(ctx, a.proc_key, proc);
out_free:
	if (projection)
		mtree_destroy(&projection->admission);
	kvfree(projection);
	return ret;
}

static unsigned long
flux_projection_range_end(struct flux_projection *projection,
			  unsigned long address)
{
	MA_STATE(mas, &projection->admission, address, address);
	unsigned long end = 0;

	if (!projection->admitted_pages)
		return 0;
	rcu_read_lock();
	if (mas_walk(&mas))
		end = mas.last + 1;
	rcu_read_unlock();
	return end;
}

static bool
flux_projection_table_valid(const struct flux_projection *projection,
			    unsigned long address)
{
	return !(address & ~PAGE_MASK) && address >= projection->memory_start &&
	       address <= projection->memory_end - PAGE_SIZE;
}

/* Alias lock excludes completion of guest table reclamation, not PTE updates. */
static unsigned long
flux_projection_leaf_table(const struct flux_projection *projection,
			   unsigned long address)
{
	unsigned long table = projection->pgd;
	u64 entry;
	int shift;

	for (shift = 39; shift > PAGE_SHIFT; shift -= 9) {
		unsigned long slot = ((address >> shift) & 511) * sizeof(entry);

		if (!flux_projection_table_valid(projection, table) ||
		    copy_from_user_nofault(&entry,
					   (void __user *)(table + slot),
					   sizeof(entry)) ||
		    !(entry & FLUX_PROJECTION_PT_PRESENT) ||
		    (entry & FLUX_PROJECTION_PT_HUGE))
			return 0;
		table = entry & PAGE_MASK;
	}
	return flux_projection_table_valid(projection, table) ? table : 0;
}

/* Return with the host mmap write lock held only on success. */
static struct vm_area_struct *flux_projection_reserve(struct mm_struct *mm,
						      unsigned long address,
						      unsigned int *nr)
{
	struct vm_area_struct *vma;
	unsigned long end = address + (*nr << PAGE_SHIFT);
	unsigned long reserved;

	mmap_write_lock(mm);
	vma = find_vma(mm, address);
	if (!vma || address < vma->vm_start) {
		if (vma)
			end = min(end, vma->vm_start);
		mmap_write_unlock(mm);
		reserved = flux_alias_reserve_range(address, end - address,
						    false, false);
		if (reserved != address)
			return NULL;
		mmap_write_lock(mm);
		vma = find_vma(mm, address);
	}
	if (!vma || address < vma->vm_start ||
	    !flux_alias_reservation_vma(vma) || (vma->vm_flags & VM_SHARED)) {
		mmap_write_unlock(mm);
		return NULL;
	}
	*nr = min_t(unsigned int, *nr, (vma->vm_end - address) >> PAGE_SHIFT);
	return vma;
}

/* Pin a readable prefix before taking the target mmap write lock. */
static unsigned int flux_projection_pin_pages(
	struct mm_struct *mm, const struct flux_projection *projection,
	const u64 *leaves, unsigned int nr, struct page **pages)
{
	unsigned int i = 0;
	int locked = 1;

	if (!mmap_read_trylock(mm))
		return 0;
	while (i < nr) {
		unsigned long backing = leaves[i] & PAGE_MASK;
		unsigned int run = 0;
		long pinned;

		while (i + run < nr) {
			u64 leaf = leaves[i + run];

			if ((leaf & (FLUX_PROJECTION_PT_PRESENT |
				     FLUX_PROJECTION_PT_USER |
				     FLUX_PROJECTION_PT_NONE)) !=
				    (FLUX_PROJECTION_PT_PRESENT |
				     FLUX_PROJECTION_PT_USER) ||
			    !flux_projection_table_valid(projection,
							 leaf & PAGE_MASK) ||
			    (leaf & PAGE_MASK) != backing + run * PAGE_SIZE)
				break;
			run++;
		}
		if (!run)
			break;
		pinned = pin_user_pages_remote(mm, backing, run,
					       FOLL_LONGTERM | FOLL_WRITE |
						       FOLL_NOWAIT,
					       pages + i, &locked);
		if (pinned <= 0)
			break;
		i += pinned;
		if (!locked || pinned != run)
			break;
	}
	if (locked)
		mmap_read_unlock(mm);
	return i;
}

static bool flux_projection_fault(struct flux_mm_ctx *ctx,
				  unsigned long address)
{
	struct mm_struct *mm = current->mm;
	struct flux_projection *projection;
	struct flux_alias_mm *alias_mm;
	struct vm_area_struct *vma;
	unsigned long table, slot, end;
	u64 *leaves;
	struct page **pages;
	unsigned int nr, pinned, i;
	bool repaired = false;

	address &= PAGE_MASK;
	if (!mutex_trylock(flux_alias_mm_lock(mm)))
		return false;
	alias_mm = flux_alias_mm_find_locked(ctx, mm);
	if (!alias_mm || alias_mm->released || !alias_mm->projection) {
		atomic_long_inc(&fault_no_domain);
		goto out_alias;
	}
	projection = alias_mm->projection;
	leaves = projection->leaves;
	pages = projection->pages;
	end = flux_projection_range_end(projection, address);
	if (!end) {
		atomic_long_inc(&fault_no_range);
		goto out_alias;
	}
	table = flux_projection_leaf_table(projection, address);
	if (!table) {
		atomic_long_inc(&fault_bad_walk);
		goto out_alias;
	}
	slot = (address >> PAGE_SHIFT) & 511;
	/* Ramp up only for a sequential stream; sparse/random reads stay scalar. */
	nr = address == projection->next_read ?
		     min(fork_fault_batch,
			 max(1U, projection->read_window * 2)) :
		     1;
	nr = min_t(unsigned long, nr, 512 - slot);
	nr = min_t(unsigned long, nr, (end - address) >> PAGE_SHIFT);
	if (copy_from_user_nofault(leaves, (void __user *)(table + slot * 8),
				   nr * sizeof(*leaves)))
		goto out_alias;
	/* A non-readable first leaf is a real guest fault, not projection work. */
	if ((leaves[0] & (FLUX_PROJECTION_PT_PRESENT | FLUX_PROJECTION_PT_USER |
			  FLUX_PROJECTION_PT_NONE)) !=
	    (FLUX_PROJECTION_PT_PRESENT | FLUX_PROJECTION_PT_USER))
		goto out_alias;
	pinned = flux_projection_pin_pages(mm, projection, leaves, nr, pages);
	if (!pinned) {
		atomic_long_inc(&fault_no_pin);
		goto out_alias;
	}
	nr = pinned;
	vma = flux_projection_reserve(mm, address, &nr);
	if (!vma) {
		atomic_long_inc(&fault_no_reservation);
		goto out_pins;
	}
	for (i = 0; i < nr; i++) {
		struct flux_alias_args a = {
			.user_va = address + i * PAGE_SIZE,
			.nr = 1,
			.prot = PROT_READ,
			.flags = FLUX_ALIAS_F_CHECK_PTE,
			.expected_pte_addr = table + (slot + i) * sizeof(u64),
			.expected_pte = leaves[i],
			.expected_pte_mask = ~0ULL,
		};
		struct flux_alias_source *source, *old;
		bool absent = false;
		int ret;

		/* Never downgrade already installed neighboring aliases. */
		if (i && xa_load(&alias_mm->aliases, a.user_va >> PAGE_SHIFT))
			continue;
		source = flux_alias_source_take(ctx, pages[i], GFP_KERNEL);
		pages[i] = NULL;
		if (IS_ERR(source))
			break;
		ret = flux_alias_track_source_locked(ctx, mm, a.user_va, source,
						     GFP_KERNEL, &old);
		if (ret)
			break;
		ret = flux_alias_install_pfn_locked(ctx, vma, a.user_va,
						    page_to_pfn(source->page),
						    &a, &absent);
		if (ret) {
			flux_alias_restore_source_locked(ctx, mm, a.user_va,
							 absent ? NULL : old);
			if (absent)
				flux_alias_source_put(old);
			atomic_long_inc(&fault_failed);
			break;
		}
		flux_alias_source_put(old);
		atomic_long_inc(&fault_pages);
		if (!i)
			repaired = true;
	}
	mmap_write_unlock(mm);
	if (repaired) {
		projection->read_window = max(i, 1U);
		projection->next_read = address + (max(i, 1U) << PAGE_SHIFT);
		atomic_long_inc(&fault_repaired);
		current->min_flt++;
	}
out_pins:
	for (i = 0; i < pinned; i++) {
		if (pages[i])
			unpin_user_page(pages[i]);
	}
out_alias:
	mutex_unlock(flux_alias_mm_lock(mm));
	return repaired;
}

static void (*original_bad_area)(struct pt_regs *, unsigned long, unsigned long,
				 u32, int);

static void flux_bad_area(struct pt_regs *regs, unsigned long error,
			  unsigned long address, u32 pkey, int code)
{
	struct flux_mm_ctx *ctx = NULL;
	bool repaired = false;

	/* Ordinary CPL3 data reads only: never COW, execute, PKRU or uaccess. */
	if (READ_ONCE(fork_fault_repair) && user_mode(regs) && (error & 4) &&
	    !(error & ~0x5UL) && address < FLUX_PHYS_BASE &&
	    !flux_kernel_alias(address) &&
	    (code == SEGV_MAPERR || code == SEGV_ACCERR))
		ctx = flux_mm_ctx_get_current_rcu();
	if (ctx) {
		local_irq_enable();
		if (!READ_ONCE(ctx->mpk_enabled))
			repaired = flux_projection_fault(ctx, address);
		flux_mm_ctx_put(ctx);
		if (repaired)
			local_irq_disable();
		else
			atomic_long_inc(&fault_fallback);
	}
	if (!repaired)
		original_bad_area(regs, error, address, pkey, code);
	atomic_dec(&fault_hook_calls);
}

static struct flux_ftrace_hook projection_hooks[] = {
	FLUX_GLOBAL_TRACKED_HOOK("__bad_area_nosemaphore", flux_bad_area,
				 &original_bad_area, &fault_hook_calls),
};
static struct flux_hook_group projection_group = {
	.name = "fork projection fault",
	.hooks = projection_hooks,
	.nr_hooks = ARRAY_SIZE(projection_hooks),
};

int flux_projection_hook_init(void)
{
	if (!fork_fault_batch || fork_fault_batch > FLUX_PROJECTION_BATCH_MAX)
		return -EINVAL;
	return fork_fault_repair ? flux_hook_group_install(&projection_group) :
				   0;
}

void flux_projection_hook_exit(void)
{
	WRITE_ONCE(fork_fault_repair, false);
	flux_hook_group_remove(&projection_group);
	while (atomic_read(&fault_hook_calls))
		usleep_range(1000, 2000);
	WARN_ON_ONCE(atomic_long_read(&fault_pending));
}
