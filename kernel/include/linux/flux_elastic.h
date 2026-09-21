/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FLUX_ELASTIC_H
#define _LINUX_FLUX_ELASTIC_H

#include <linux/types.h>

struct page;
struct zone;

#ifdef CONFIG_FLUX_ELASTIC_MEMORY
#include <asm/elastic.h>
#else
/* Disabled builds keep the ordinary allocator path without runtime dispatch. */
static inline struct page *flux_elastic_alloc(struct zone *zone,
					    unsigned int order, gfp_t gfp)
{
	return NULL;
}

static inline bool flux_elastic_contains(struct page *page)
{
	return false;
}

static inline bool flux_elastic_free(struct page *page, unsigned int order)
{
	return false;
}
#endif

#endif /* _LINUX_FLUX_ELASTIC_H */
