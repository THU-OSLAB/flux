/*
 * core.c - core networking infrastructure
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <asm/host_ops.h>
#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <net/arp.h>
#include <net/ip.h>

#include "fnet.h"
#include "trans.h"
#include "core.h"
#include "csum.h"
#include "ip.h"
#include "local_cache.h"

#define IP_ID_SEED 0x42345323
#define RX_PREFETCH_STRIDE 2
#define RX_MBUF_CPU_CACHE_SIZE 256

static DEFINE_PER_CPU(struct fnet_local_cache, rx_mbuf_cpu_caches);
static struct kmem_cache *rx_mbuf_cache;

static int net_rx_mbuf_cache_init(void)
{
	rx_mbuf_cache = kmem_cache_create("rx_mbuf_cache", MBUF_HEAD_LEN, 0,
					 SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
	return rx_mbuf_cache ? 0 : -ENOMEM;
}
core_initcall(net_rx_mbuf_cache_init);

static inline void net_rx_send_comp(unsigned long offset)
{
	struct fnet_cpu *cpu = fnet_get_cpu();
	union flux_txcmdq_cmd cmd = { .txcmd = FLUX_TXCMD_NET_COMP };

	if (unlikely(!lrpc_send(&cpu->txcmdq, cmd.lrpc_cmd, offset)))
		BUG();
	fnet_put_cpu();
}

static struct mbuf *__net_rx_alloc_mbuf(void)
{
	struct mbuf *m;

	m = fnet_local_cache_pop(&rx_mbuf_cpu_caches);

	return m ?: kmem_cache_alloc(rx_mbuf_cache, GFP_KERNEL);
}

static void __net_rx_free_mbuf(struct mbuf *m)
{
	unsigned long offset = m->release_data;

	net_rx_send_comp(offset);

	if (unlikely(!fnet_local_cache_push(&rx_mbuf_cpu_caches, m,
					    RX_MBUF_CPU_CACHE_SIZE)))
		kmem_cache_free(rx_mbuf_cache, m);
}

struct mbuf *net_rx_alloc_mbuf(unsigned long data, union flux_rxq_cmd cmd)
{
	struct mbuf *m = NULL;
	void *src_buf;

	src_buf = fnet_rx_data_from_offset(fnet_dev, data, cmd.len);
	if (unlikely(!src_buf))
		goto out;

	/* Keep the shared RX payload until the transport releases the mbuf. */
	m = __net_rx_alloc_mbuf();
	if (unlikely(!m))
		goto out;

	mbuf_init(m, src_buf, cmd.len, 0);
	m->len = cmd.len;
	m->csum_type = cmd.csum_type;
	m->release_data = data;
	m->release = __net_rx_free_mbuf;
	return m;

out:
	net_rx_send_comp(data);
	return NULL;
}

static inline bool ip_hdr_supported(const struct iphdr *iphdr)
{
	/* must be IPv4, no IP options, no IP fragments */
	return (iphdr->version == IPVERSION &&
		iphdr->ihl == sizeof(*iphdr) / sizeof(uint32_t) &&
		(iphdr->frag_off & IP_MF) == 0);
}

static void net_rx_one(struct mbuf *m)
{
	const struct ethhdr *llhdr;
	const struct iphdr *iphdr;
	uint16_t len;

	/*
	 * Link Layer Processing (OSI L2)
	 */

	llhdr = mbuf_pull_hdr_or_null(m, *llhdr);
	if (unlikely(!llhdr))
		goto drop;

	/*
	 * Network Layer Processing (OSI L3)
	 */

	mbuf_mark_network_offset(m);
	iphdr = mbuf_pull_hdr_or_null(m, *iphdr);
	if (unlikely(!iphdr))
		goto drop;

	/* Did HW checksum verification pass? */
	if (m->csum_type != FLUX_CHKSUM_TYPE_UNNECESSARY) {
		if (chksum_internet(iphdr, sizeof(*iphdr)))
			goto drop;
	}

	if (unlikely(!ip_hdr_supported(iphdr)))
		goto drop;
	len = be16_to_cpu(iphdr->tot_len) - sizeof(*iphdr);
	if (unlikely(mbuf_length(m) < len))
		goto drop;
	if (len < mbuf_length(m))
		mbuf_trim(m, mbuf_length(m) - len);

	switch (iphdr->protocol) {
	case IPPROTO_UDP:
	case IPPROTO_TCP:
		net_rx_trans(m);
		break;
	default:
		goto drop;
	}

	return;

drop:
	mbuf_drop(m);
}

/**
 * net_rx_batch - handles a batch of ingress packets
 * @ms: an array of ingress packets
 * @nr: the size of the @ms array
 */
void net_rx_batch(struct mbuf **ms, unsigned int nr)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (i + RX_PREFETCH_STRIDE < nr)
			prefetch(ms[i + RX_PREFETCH_STRIDE]->data);
		net_rx_one(ms[i]);
	}
}

bool flux_fast_net_rx_recv(unsigned long payload, union flux_rxq_cmd cmd)
{
	struct mbuf *m;
	unsigned long offset;

	if (!flux_rxq_payload_is_fast_net(payload))
		return false;

	offset = flux_rxq_payload_to_rx_offset(payload);
	m = net_rx_alloc_mbuf(offset, cmd);
	if (unlikely(!m))
		return false;

	fnet_dbg("rx: 0x%llx len %u\n", (u64)m, cmd.len);

	net_rx_one(m);
	return true;
}

/*
 * TX Networking Functions
 */

static bool net_tx_one(struct fnet_cpu *cpu, struct mbuf *m)
{
	union flux_txpktq_cmd cmd;
	uint64_t payload;
	uint16_t len = mbuf_length(m);

	cmd.dst_ip = m->tx_dst_ip;
	cmd.txcmd = FLUX_TXPKT_NET_XMIT;
	cmd.len = len;
	cmd.olflags = m->txflags;

	payload = flux_txpkt_payload_from_ptr((uint64_t)m, FLUX_MEMORY_ADDR,
					      (uint16_t)m->hash);

	fnet_dbg("tx: mbuf 0x%llx len %u\n", (u64)m, len);

	return lrpc_send(&cpu->txpktq, cmd.lrpc_cmd, payload);
}

/* drains overflow queues */
bool net_tx_drain_ofq(struct fnet_cpu *cpu)
{
	struct mbuf *m;

	while (!mbufq_empty(&cpu->txofq_mbuf)) {
		m = mbufq_peak_head(&cpu->txofq_mbuf);
		if (unlikely(!net_tx_one(cpu, m)))
			return false;
		mbufq_pop_head(&cpu->txofq_mbuf);
	}

	return true;
}

static void net_tx_raw_iok(struct mbuf *m)
{
	struct fnet_cpu *cpu;

	cpu = fnet_get_cpu();

	/* drain pending overflow packets first */
	if (unlikely(!mbufq_empty(&cpu->txofq_mbuf))) {
		if (!net_tx_drain_ofq(cpu)) {
			mbufq_push_tail(&cpu->txofq_mbuf, m);
			goto out;
		}
	}

	if (unlikely(!net_tx_one(cpu, m)))
		mbufq_push_tail(&cpu->txofq_mbuf, m);

out:
	fnet_put_cpu();
}

static void net_tx_raw(struct mbuf *m)
{
	net_tx_raw_iok(m);
}

/**
 * net_tx_eth - transmits an ethernet packet
 * @m: the mbuf to transmit
 * @type: the ethernet type (in native byte order)
 * @dhost: the destination MAC address
 *
 * The payload must start with the network (L3) header. The ethernet (L2)
 * header will be prepended by this function.
 *
 * @m must have been allocated with net_tx_alloc_mbuf().
 */
void net_tx_eth(struct mbuf *m, uint16_t type, const struct ethaddr *dhost)
{
	struct ethhdr *hdr;
	hdr = mbuf_push_hdr(m, *hdr);
	ether_addr_copy(hdr->h_source, fnet_dev->fast_net_mac);
	ether_addr_copy(hdr->h_dest, dhost->addr);
	hdr->h_proto = htons(type);
	net_tx_raw(m);
}

static void net_push_iphdr(struct mbuf *m, uint8_t proto, uint32_t daddr)
{
	struct iphdr *iphdr;

	/* populate IP header */
	iphdr = mbuf_push_hdr(m, *iphdr);
	iphdr->version = IPVERSION;
	iphdr->ihl = 5;
	iphdr->tos = IPTOS_DSCP_CS0 | IPTOS_ECN_NOTECT;
	iphdr->tot_len = htons(mbuf_length(m));
	iphdr->id = 0; /* see RFC 6864 */
	iphdr->frag_off = htons(IP_DF);
	iphdr->ttl = 64;
	iphdr->protocol = proto;
	iphdr->check = 0;
	iphdr->saddr = htonl(fnet_dev->fast_net_ip);
	iphdr->daddr = htonl(daddr);

	if (!fnet_dev->arg.csum_offload)
		iphdr->check = ipv4_cksum(iphdr);
}

/* simple IP routing */
static uint32_t net_get_ip_route(uint32_t daddr)
{
	if ((daddr & fnet_dev->fast_net_netmask) !=
	    (fnet_dev->fast_net_ip & fnet_dev->fast_net_netmask))
		daddr = fnet_dev->fast_net_gateway;
	return daddr;
}

static bool net_lookup_valid_neigh(__be32 daddr, struct ethaddr *dhost)
{
	struct neighbour *neigh;
	bool found = false;

	rcu_read_lock();
	neigh = __ipv4_neigh_lookup_noref(fnet_dev->dev, (__force u32)daddr);
	if (likely(neigh && (READ_ONCE(neigh->nud_state) & NUD_VALID))) {
		ether_addr_copy(dhost->addr, neigh->ha);
		found = true;
	}
	rcu_read_unlock();

	return found;
}

static int net_probe_neigh(uint32_t daddr, struct ethaddr *dhost)
{
	struct neighbour *neigh;
	const u64 retry_delay = 100; /* in micros */
	int retry_cnt = 0;
	__be32 daddr_be;
	int ret;

	daddr_be = htonl(daddr);
	if (likely(net_lookup_valid_neigh(daddr_be, dhost)))
		return 0;
retry:
	neigh = neigh_lookup(&arp_tbl, &daddr_be, fnet_dev->dev);
	if (neigh) {
		if (neigh->nud_state & NUD_VALID) {
			ether_addr_copy(dhost->addr, neigh->ha);
			ret = 0;
		} else {
			neigh_event_send(neigh, NULL);
			neigh_release(neigh);
			udelay(retry_delay);
			goto retry;
		}
		neigh_release(neigh);
	} else {
		neigh = neigh_create(&arp_tbl, &daddr_be, fnet_dev->dev);
		if (!IS_ERR(neigh)) {
			neigh_event_send(neigh, NULL);
			neigh_release(neigh);
		} else {
			ret = PTR_ERR(neigh);
			goto done;
		}
		if (++retry_cnt < 5) {
			udelay(retry_delay);
			goto retry;
		}
		ret = -EHOSTUNREACH;
	}
done:
	return ret;
}

/**
 * net_tx_ip - transmits an IP packet
 * @m: the mbuf to transmit
 * @proto: the transport protocol
 * @daddr: the destination IP address (in native byte order)
 *
 * The payload must start with the transport (L4) header. The IPv4 (L3) and
 * ethernet (L2) headers will be prepended by this function.
 *
 * @m must have been allocated with net_tx_alloc_mbuf().
 *
 * Returns 0 if successful. If successful, the mbuf will be freed when the
 * transmit completes. Otherwise, the mbuf still belongs to the caller.
 */
int net_tx_ip(struct mbuf *m, uint8_t proto, uint32_t daddr)
{
	struct ethaddr dhost;
	int ret;

	/* prepend the IP header */
	net_push_iphdr(m, proto, daddr);
	mbuf_mark_network_offset(m);

	/* ask NIC to calculate IP checksum */
	m->txflags |= FLUX_OLFLAG_IP_CHKSUM | FLUX_OLFLAG_IPV4;

	/* apply IP routing */
	daddr = net_get_ip_route(daddr);

	/* need to use ARP to resolve dhost */
	ret = net_probe_neigh(daddr, &dhost);
	if (unlikely(ret)) {
		/* An unrecoverable error occurred */
		mbuf_pull_hdr(m, struct iphdr);
		return ret;
	}

	net_tx_eth(m, ETH_P_IP, &dhost);
	return 0;
}
