/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_ELASTIC_H
#define _ASM_FLUX_ELASTIC_H
#include <linux/types.h>
struct page;
struct zone;
void flux_elastic_bootmem(unsigned long base, unsigned long size);
struct page *flux_elastic_alloc(struct zone *zone, unsigned int order, gfp_t gfp);
bool flux_elastic_contains(struct page *page);
bool flux_elastic_free(struct page *page, unsigned int order);

struct flux_elastic_snapshot {
	unsigned int capacity_mb;
	unsigned int target_mb;
	unsigned int resident_mb;
	unsigned int free_pages;
	bool enabled;
};

int flux_elastic_resize(unsigned int mb, unsigned int migration_budget);
void flux_elastic_snapshot(struct flux_elastic_snapshot *snapshot);

#endif
