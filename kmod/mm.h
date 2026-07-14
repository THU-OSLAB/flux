#ifndef _FLUX_HOOK_H
#define _FLUX_HOOK_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/xarray.h>

#define TIF_HOOK_ACTIVE 30

extern int flux_mm_hook_init(void);
extern void flux_mm_hook_exit(void);
extern int flux_mm_hook_install(void);
extern int flux_mm_hook_remove(void);

extern int flux_execve(unsigned long arg);

extern void flux_dump_vmas(void);

struct flux_proc {
	int key;
	/* A single counter is enough */
	atomic_t count;

	/* Each bound to a specific mm_struct */
	struct mm_struct *mm;

	/* MM fault and swap info: this can arguably be seen as
    either mm-specific or thread-specific: */
	unsigned long min_flt;
	unsigned long maj_flt;
	/* Context switch counts: */
	unsigned long nvcsw;
	unsigned long nivcsw;
};

struct flux_mm_ctx {
	struct mutex lock;
	struct xarray procs;
	struct flux_proc proc0;
	struct kref refcount;
	pid_t owner_tgid;
	struct mm_struct *clean_mm;
	struct list_head base_maps;
	struct list_head clean_maps;
	struct list_head registry_node;
	bool mpk_enabled;
};

#define flux_proc_xa_limit XA_LIMIT(1, UINT_MAX)

extern int flux_copy_mm(struct flux_mm_ctx *ctx, int __user *proc_key_ptr);
extern int flux_release_mm(struct flux_mm_ctx *ctx, int proc_key);
extern int flux_switch_mm(struct flux_mm_ctx *ctx, int proc_key_to,
			  int proc_key_from);
extern int flux_clean_mm(struct flux_mm_ctx *ctx, int cmd);

static inline void flux_proc_get(struct flux_proc *p)
{
	atomic_inc(&p->count);
}
void flux_proc_put(struct flux_mm_ctx *ctx, int proc_key,
		   struct flux_proc *proc);

extern int flux_mm_setup_ctx(struct file *filp);
extern void flux_mm_ctx_release(struct kref *ref);
struct flux_mm_ctx *flux_mm_ctx_get_current(void);
void flux_mm_ctx_put(struct flux_mm_ctx *ctx);
bool flux_mm_mpk_enabled_current_rcu(void);

#endif
