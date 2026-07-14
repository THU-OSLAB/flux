#ifndef _FLUX_FNET_LOCAL_CACHE_H
#define _FLUX_FNET_LOCAL_CACHE_H

#include <linux/percpu.h>
#include <linux/preempt.h>

#include "mbuf.h"

struct fnet_local_cache {
	struct mbuf *head;
	unsigned int count;
};

#ifdef CONFIG_FLUX_FAST_NET_LOCAL_CACHE
static inline struct mbuf *
fnet_local_cache_pop(struct fnet_local_cache __percpu *caches)
{
	struct fnet_local_cache *cache;
	struct mbuf *m;

	preempt_disable();
	cache = this_cpu_ptr(caches);
	m = cache->head;
	if (likely(m)) {
		cache->head = m->next;
		cache->count--;
	}
	preempt_enable();

	return m;
}

static inline bool
fnet_local_cache_push(struct fnet_local_cache __percpu *caches,
		      struct mbuf *m, unsigned int max_count)
{
	struct fnet_local_cache *cache;
	bool cached = false;

	preempt_disable();
	cache = this_cpu_ptr(caches);
	if (likely(cache->count < max_count)) {
		m->next = cache->head;
		cache->head = m;
		cache->count++;
		cached = true;
	}
	preempt_enable();

	return cached;
}
#else
static inline struct mbuf *
fnet_local_cache_pop(struct fnet_local_cache __percpu *caches)
{
	(void)caches;
	return NULL;
}

static inline bool
fnet_local_cache_push(struct fnet_local_cache __percpu *caches,
		      struct mbuf *m, unsigned int max_count)
{
	(void)caches;
	(void)m;
	(void)max_count;
	return false;
}
#endif

#endif /* _FLUX_FNET_LOCAL_CACHE_H */
