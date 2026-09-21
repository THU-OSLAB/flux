/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FLUX_ALIAS_INTERNAL_H
#define _FLUX_ALIAS_INTERNAL_H

#include "internal.h"

struct flux_alias_source {
	struct page *page;
	struct xarray *registry;
	refcount_t refs;
};

struct flux_projection;

struct flux_alias_mm {
	struct list_head node;
	struct mm_struct *mm;
	struct xarray aliases;
	struct flux_projection *projection;
	unsigned int gate_refs;
	bool released;
};

/* All target-index operations require flux_alias_mm_lock(mm). */
struct flux_alias_mm *flux_alias_mm_find_locked(struct flux_mm_ctx *ctx,
						struct mm_struct *mm);
struct flux_alias_mm *flux_alias_mm_get_locked(struct flux_mm_ctx *ctx,
					       struct mm_struct *mm, gfp_t gfp);
void flux_alias_source_put(struct flux_alias_source *source);
struct flux_alias_source *flux_alias_source_take(struct flux_mm_ctx *ctx,
					       struct page *page, gfp_t gfp);
int flux_alias_track_source_locked(struct flux_mm_ctx *ctx,
				   struct mm_struct *mm, unsigned long addr,
				   struct flux_alias_source *source, gfp_t gfp,
				   struct flux_alias_source **old_source);
int flux_alias_install_pfn_locked(struct flux_mm_ctx *ctx,
				  struct vm_area_struct *vma, unsigned long uva,
				  unsigned long pfn,
				  const struct flux_alias_args *a,
				  bool *alias_absent);
void flux_alias_restore_source_locked(struct flux_mm_ctx *ctx,
				      struct mm_struct *mm, unsigned long addr,
				      struct flux_alias_source *old_source);
void flux_projection_mm_release(struct flux_alias_mm *alias_mm);
void flux_projection_invalidate_locked(struct flux_alias_mm *alias_mm,
				       unsigned long start, unsigned long end);

#endif
