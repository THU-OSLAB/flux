/*
 * tx_mbuf.c - shared TX mbuf allocator
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <linux/etherdevice.h>
#include <linux/slab.h>

#include "fnet.h"
#include "core.h"
#include "local_cache.h"

#define TX_MBUF_CPU_CACHE_SIZE 128

static DEFINE_PER_CPU(struct fnet_local_cache, tx_mbuf_cpu_caches);

static inline size_t default_buf_size(void)
{
	return ALIGN(FLUX_FNET_MTU + eth_headroom() + MBUF_HEAD_LEN,
		     L1_CACHE_BYTES * 2);
}

static struct kmem_cache *tx_mbuf_cache;

static struct mbuf *__net_tx_alloc_mbuf(void)
{
	struct mbuf *m;

	m = fnet_local_cache_pop(&tx_mbuf_cpu_caches);

	if (unlikely(!m))
		m = kmem_cache_alloc(tx_mbuf_cache, GFP_KERNEL);
	if (m)
		m->release_data = (unsigned long)tx_mbuf_cache;
	return m;
}

static void __net_tx_free_mbuf(struct mbuf *m)
{
	if (unlikely(!fnet_local_cache_push(&tx_mbuf_cpu_caches, m,
					    TX_MBUF_CPU_CACHE_SIZE)))
		kmem_cache_free((struct kmem_cache *)m->release_data, m);
}

static int net_tx_mbuf_cache_init(void)
{
	tx_mbuf_cache = kmem_cache_create(
		"tx_mbuf_cache", default_buf_size(), 0,
		SLAB_CACHE_DMA | SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
	return tx_mbuf_cache ? 0 : -ENOMEM;
}
core_initcall(net_tx_mbuf_cache_init);

void net_tx_release_mbuf(struct mbuf *m)
{
	__net_tx_free_mbuf(m);
}

struct mbuf *net_tx_alloc_mbuf(size_t header_len)
{
	struct mbuf *m;
	unsigned char *buf;

	m = __net_tx_alloc_mbuf();
	if (unlikely(!m)) {
		pr_warn("out of mbufs\n");
		return NULL;
	}

	buf = (unsigned char *)m + MBUF_HEAD_LEN;
	mbuf_init(m, buf, FLUX_FNET_MTU + eth_headroom(), header_len);
	m->txflags = 0;
	m->release = net_tx_release_mbuf;
	return m;
}
