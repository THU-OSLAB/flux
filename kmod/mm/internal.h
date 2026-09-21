#ifndef _FLUX_KMOD_MM_INTERNAL_H
#define _FLUX_KMOD_MM_INTERNAL_H

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagewalk.h>
#include <linux/pid.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include <asm/mmu_context.h>
#include <asm/pkeys.h>
#include <asm/syscall.h>

#include "../compat/mm.h"
#include "../dev.h"
#include "../hook.h"
#include "../mm.h"
#include "../mpk.h"

/* VMA snapshots and smaps accounting. */
struct flux_mm_range {
	unsigned long start;
	unsigned long end;
	unsigned long pgoff;
	unsigned long flags;
	struct file *file;
	struct list_head list;
};

#define FLUX_SMAPS_PSS_SHIFT 12
#define FLUX_PHYS_BASE 0x700000000000UL
#define FLUX_VMALLOC_START 0x100000000000UL
#define FLUX_VMALLOC_END 0x200000000000UL

/* Per-process alias gate state. */
enum flux_alias_state {
	FLUX_ALIAS_STATE_NONE,
	FLUX_ALIAS_STATE_PENDING,
	FLUX_ALIAS_STATE_COPYING,
	FLUX_ALIAS_STATE_ACTIVE,
};

/* Context registry and process-slot allocation. */
extern struct list_head flux_mm_ctx_registry;
extern struct mutex flux_mm_ctx_registry_lock;
extern struct kmem_cache *flux_proc_cache;

/* Host kernel symbols resolved during module initialization. */
extern int (*flux_walk_page_vma)(struct vm_area_struct *vma,
				 const struct mm_walk_ops *ops, void *private);
extern int (*flux_smaps_pte_range)(pmd_t *pmd, unsigned long start,
				   unsigned long end, struct mm_walk *walk);
extern long (*flux_sys_execve)(struct pt_regs *regs);
extern struct mm_struct *(*flux_dup_mm)(struct task_struct *task,
					struct mm_struct *oldmm);
extern void (*flux_switch_mm_irqs_off)(struct mm_struct *prev,
				       struct mm_struct *next,
				       struct task_struct *task);
extern void (*flux_flush_tlb_mm_range)(struct mm_struct *mm,
				       unsigned long start, unsigned long end,
				       unsigned int stride_shift,
				       bool freed_tables);
#ifdef CONFIG_SCHED_MM_CID
extern void (*flux_sched_mm_cid_before_execve)(struct task_struct *task);
#endif
#ifdef CONFIG_MEMBARRIER
extern void (*flux_membarrier_update_current_mm)(struct mm_struct *mm);
#endif
extern const struct vm_special_mapping *flux_vdso_mapping;
extern const struct vm_special_mapping *flux_vvar_mapping;
extern const struct vm_special_mapping *flux_vclock_mapping;
extern const struct vm_operations_struct *flux_vma_dummy_vm_ops;
extern flux_host_munmap_fn flux_do_munmap;

extern int flux_mm_resolve_symbols(void);

/* Alias address classification. */
static inline bool flux_kernel_alias(unsigned long addr)
{
	return addr >= FLUX_VMALLOC_START && addr < FLUX_VMALLOC_END;
}

/* VMA snapshots and smaps accounting. */
extern struct mm_walk_ops flux_smaps_walk_ops;
extern void flux_mm_ranges_clear(struct list_head *head);
extern bool flux_mm_range_list_overlaps(struct list_head *head,
					unsigned long start, unsigned long end);
extern struct flux_mm_range *flux_mm_range_from_vma(struct vm_area_struct *vma,
						    gfp_t gfp);
extern int flux_mm_record_base_maps(struct flux_mm_ctx *ctx,
				    struct mm_struct *mm);
extern void flux_mm_ctx_reset_ranges(struct flux_mm_ctx *ctx);

/* Process-slot lifetime and accounting. */
extern struct flux_proc *flux_proc_alloc(gfp_t flags);
extern void flux_proc_free(struct flux_proc *proc);
extern void flux_proc_init(struct flux_proc *proc, int count,
			   struct mm_struct *mm);
extern void flux_proc_release(struct flux_mm_ctx *ctx, int proc_key,
			      struct flux_proc *proc);
extern void flux_proc_save_stats(struct flux_proc *proc,
				 const struct task_struct *task);
extern void flux_proc_restore_stats(const struct flux_proc *proc,
				    struct task_struct *task);
extern struct flux_proc *flux_proc_get_by_key(struct flux_mm_ctx *ctx,
					      int proc_key);

/* Alias reservations and fork serialization. */
extern void flux_alias_locks_init(void);
extern bool flux_alias_reservation_vma(const struct vm_area_struct *vma);
extern unsigned long flux_alias_reserve_range(unsigned long uva,
					      unsigned long len, bool shared,
					      bool replace);
extern int flux_alias_gate_acquire_mm(struct flux_mm_ctx *ctx,
				      struct mm_struct *mm);
extern int flux_alias_gate_release(struct flux_mm_ctx *ctx,
				   struct mm_struct *mm);

/* Inactive execution-MM cache. */
extern struct mm_struct *flux_fork_mm_get(struct flux_mm_ctx *ctx,
					  struct mm_struct *parent);
extern bool flux_fork_mm_put(struct flux_mm_ctx *ctx, struct flux_proc *proc);
extern void flux_fork_mm_drain(struct flux_mm_ctx *ctx);

/* Runtime stack setup. */
extern void flux_remap_stack(void);

#endif /* _FLUX_KMOD_MM_INTERNAL_H */
