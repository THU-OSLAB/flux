#ifndef _FLUX_KMOD_MM_H
#define _FLUX_KMOD_MM_H

#include <linux/atomic.h>
#include <linux/hash.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct file;
struct mm_struct;
struct vm_area_struct;
struct flux_fork_mm_entry;

#define TIF_HOOK_ACTIVE 30

#define FLUX_ALIAS_LOCK_BITS 6
#define FLUX_ALIAS_LOCK_COUNT (1U << FLUX_ALIAS_LOCK_BITS)

#define FLUX_FORK_MM_CACHE_SIZE 8
#define FLUX_FORK_MM_RETRY_SIZE 4
#define flux_proc_xa_limit XA_LIMIT(1, UINT_MAX)

struct flux_fork_mm_retry {
	struct mm_struct *parent;
	unsigned int remaining;
};

struct flux_proc {
	int key;
	refcount_t refs;
	bool released;
	/* Immutable construction intent, not a readiness or gate state. */
	bool for_fork;
	/* Structural reference identifies the parent without retaining its VMAs. */
	struct mm_struct *fork_parent;
	struct rcu_head rcu;

	atomic_t alias_state;
	/* Parent host mm whose private aliases this fork gate protects. */
	struct mm_struct *alias_gate_mm;

	/* Host mm_struct bound to this Flux process slot. */
	struct mm_struct *mm;

	/* Fault and swap accounting for the bound host mm. */
	unsigned long min_flt;
	unsigned long maj_flt;
	/* Context-switch counters. */
	unsigned long nvcsw;
	unsigned long nivcsw;
};

struct flux_mm_ctx {
	struct mutex lock;
	struct xarray procs;
	int next_proc_key;
	struct flux_proc proc0;
	struct kref refcount;
	pid_t owner_tgid;
	struct list_head base_maps;
	/* Runtime mappings captured before application image construction. */
	struct mm_struct *exec_mm;
	/* Long-term pins that keep raw PFN aliases migration-safe. */
	struct list_head alias_mms[FLUX_ALIAS_LOCK_COUNT];
	/* Weak index of independently held source pages, keyed by host PFN. */
	struct xarray alias_sources;
	struct list_head registry_node;
	bool mpk_enabled;
	spinlock_t fork_cache_lock;
	struct flux_fork_mm_entry *fork_cache[FLUX_FORK_MM_CACHE_SIZE];
	bool fork_cache_stopping;
	unsigned int fork_cache_next;
	struct flux_fork_mm_retry fork_retry[FLUX_FORK_MM_RETRY_SIZE];
	unsigned int fork_retry_next;
};

/* Host hook lifetime. */
extern int flux_mm_hook_init(void);
extern void flux_mm_hook_exit(void);
extern int flux_mm_hook_install(void);
extern int flux_mm_hook_remove(void);

/* Context and process-slot ownership. */
extern int flux_mm_setup_ctx(struct file *filp);
extern void flux_mm_ctx_release(struct kref *ref);
extern struct flux_mm_ctx *flux_mm_ctx_get_current(void);
extern struct flux_mm_ctx *flux_mm_ctx_get_current_rcu(void);
extern void flux_mm_ctx_put(struct flux_mm_ctx *ctx);
extern bool flux_mm_mpk_enabled_current_rcu(void);
extern void flux_proc_put(struct flux_mm_ctx *ctx, int proc_key,
			  struct flux_proc *proc);

/* Execution-MM lifecycle. */
extern int flux_copy_mm(struct flux_mm_ctx *ctx, int __user *proc_key_ptr,
			bool for_fork);
extern int flux_release_mm(struct flux_mm_ctx *ctx, int proc_key);
extern int flux_switch_mm(struct flux_mm_ctx *ctx, int proc_key_to,
			  int proc_key_from);
extern int flux_execve(unsigned long arg);
extern int flux_fork_alias_begin(struct flux_mm_ctx *ctx, int proc_key);
extern int flux_fork_alias_end(struct flux_mm_ctx *ctx, int proc_key);

/* Alias serialization and mapping operations. */
extern struct mutex flux_alias_locks[FLUX_ALIAS_LOCK_COUNT];

static inline unsigned int flux_alias_bucket(struct mm_struct *mm)
{
	return hash_ptr(mm, FLUX_ALIAS_LOCK_BITS);
}

static inline struct mutex *flux_alias_mm_lock(struct mm_struct *mm)
{
	return &flux_alias_locks[flux_alias_bucket(mm)];
}

extern int flux_alias_pages(struct flux_mm_ctx *ctx, unsigned long arg);
extern int flux_prepare_alias_reservation(struct flux_mm_ctx *ctx,
					  unsigned long arg);
extern bool flux_alias_range_has_pkey(struct vm_area_struct *vma,
				      unsigned long start, unsigned long end,
				      int pkey);
extern int flux_unalias_pages(struct flux_mm_ctx *ctx, unsigned long arg);
extern int flux_unalias_user_mm(struct flux_mm_ctx *ctx, int proc_key);
extern int flux_flush_user_mm(struct flux_mm_ctx *ctx, int proc_key);
extern int flux_unalias_kernel_pages(struct flux_mm_ctx *ctx,
				     unsigned long arg);
extern int flux_alias_munmap_range(struct flux_mm_ctx *ctx, unsigned long start,
				   size_t len);
extern void flux_alias_release_mm(struct flux_mm_ctx *ctx,
				  struct mm_struct *mm);
extern int flux_rekey_aliases(unsigned long arg);

/* Optional fault projections. */
extern int flux_projection_hook_init(void);
extern void flux_projection_hook_exit(void);
extern int flux_bind_fork_projection(struct flux_mm_ctx *ctx, unsigned long arg);

/* Mapping observations. */
extern int flux_mm_get_base_maps(struct flux_mm_ctx *ctx, unsigned long arg);
extern int flux_mm_get_app_maps(struct flux_mm_ctx *ctx, unsigned long arg);
extern int flux_mm_get_app_smaps(struct flux_mm_ctx *ctx, unsigned long arg);

/* Coherent control and elastic backing objects. */
extern int flux_elastic_create(unsigned long arg);
extern bool flux_control_vma_shared(const struct vm_area_struct *vma);
extern bool flux_backing_vma_shared(const struct vm_area_struct *vma);

#endif /* _FLUX_KMOD_MM_H */
