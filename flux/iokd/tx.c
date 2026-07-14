#define FLUX_FMT "iokd-tx: "

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include <kernel/asm/fnet.h>
#include <kernel/asm/host_ops.h>
#include <utils/log.h>

#include "dpdk_errno.h"
#include "iokd.h"

struct flux_iokd_comp_stack {
	uint32_t size;
	uint32_t len;
	void *objs[];
};

static struct flux_iokd_pkt flux_iokd_tx_pkts[FLUX_FNET_TX_BURST_SIZE];
static int flux_iokd_tx_n_pkts;
static struct rte_mbuf *flux_iokd_tx_bufs[FLUX_FNET_TX_BURST_SIZE];
static int flux_iokd_tx_n_bufs;
static unsigned int
	flux_iokd_tx_next_cpu_rr[FLUX_IOKD_MAX_CLIENTS];

static bool flux_iokd_send_completion(void *obj)
{
	struct rte_mbuf *buf = obj;
	struct flux_iokd_tx_priv *priv = rte_mbuf_to_priv(buf);
	struct flux_iokd_client *client = priv->client;
	union flux_rxq_cmd cmd = { .rxcmd = FLUX_RX_NET_COMP };
	int cpu_idx = priv->cpu_idx;

	if (!client || !flux_iokd_client_ready(client))
		goto out_put;
	if (cpu_idx < 0 || cpu_idx >= client->nr_cpus)
		goto out_put;

	if (likely(lrpc_send(&client->pcpus[cpu_idx].rxq, cmd.lrpc_cmd,
			     priv->comp_data)))
		goto out_put;

	if (client->tx_comp_nr_ofs == client->tx_comp_max_ofs)
		goto out_put;

	client->tx_comp_ofq[client->tx_comp_nr_ofs++] = priv->comp_data;
	flux_iokd.tx_comp_pending = true;
out_put:
	priv->client = NULL;
	flux_iokd_client_put(client);
	return true;
}

static int flux_iokd_comp_enqueue(struct rte_mempool *mp,
				  void *const *obj_table, unsigned int n)
{
	struct flux_iokd_comp_stack *s = mp->pool_data;
	unsigned int i;

	if (s->len + n > s->size)
		return -ENOBUFS;

	for (i = 0; i < n; i++)
		flux_iokd_send_completion(obj_table[i]);
	for (i = 0; i < n; i++)
		s->objs[s->len + i] = obj_table[i];
	s->len += n;
	return 0;
}

static int flux_iokd_comp_dequeue(struct rte_mempool *mp, void **obj_table,
				  unsigned int n)
{
	struct flux_iokd_comp_stack *s = mp->pool_data;
	unsigned int i;
	unsigned int j;

	if (n > s->len)
		return -ENOBUFS;

	s->len -= n;
	for (i = 0, j = s->len; i < n; i++, j++)
		obj_table[i] = s->objs[j];
	return 0;
}

static unsigned int flux_iokd_comp_get_count(const struct rte_mempool *mp)
{
	struct flux_iokd_comp_stack *s = mp->pool_data;
	return s->len;
}

static int flux_iokd_comp_alloc(struct rte_mempool *mp)
{
	struct flux_iokd_comp_stack *s;
	unsigned int n = mp->size;
	size_t sz = sizeof(*s) + (n + 16) * sizeof(void *);

	s = rte_zmalloc_socket(mp->name, sz, RTE_CACHE_LINE_SIZE,
			       mp->socket_id);
	if (!s)
		return -ENOMEM;

	s->size = n;
	mp->pool_data = s;
	return 0;
}

static void flux_iokd_comp_free(struct rte_mempool *mp)
{
	rte_free(mp->pool_data);
}

static const struct rte_mempool_ops flux_iokd_comp_ops = {
	.name = "flux_iokd_completion",
	.alloc = flux_iokd_comp_alloc,
	.free = flux_iokd_comp_free,
	.enqueue = flux_iokd_comp_enqueue,
	.dequeue = flux_iokd_comp_dequeue,
	.get_count = flux_iokd_comp_get_count,
};
RTE_MEMPOOL_REGISTER_OPS(flux_iokd_comp_ops);

static void flux_iokd_tx_pktmbuf_priv_init(struct rte_mempool *mp, void *opaque,
					   void *obj, unsigned int obj_idx)
{
	struct rte_mbuf *buf = obj;
	struct flux_iokd_tx_priv *priv = rte_mbuf_to_priv(buf);

	memset(priv, 0, sizeof(*priv));
}

struct rte_mempool *flux_iokd_tx_pool_create(size_t data_room_size)
{
	struct rte_mempool *mp;
	struct rte_pktmbuf_pool_private mbp_priv = { 0 };
	unsigned int elt_size;
	int ret;

	elt_size = sizeof(struct rte_mbuf) + sizeof(struct flux_iokd_tx_priv) +
		   (unsigned int)data_room_size;
	mbp_priv.mbuf_data_room_size = (uint16_t)data_room_size;
	mbp_priv.mbuf_priv_size = sizeof(struct flux_iokd_tx_priv);

	mp = rte_mempool_create_empty("FLUX_IOKD_TX_POOL",
				      FLUX_IOK_TX_COMP_POOL_SIZE, elt_size, 0,
				      sizeof(struct rte_pktmbuf_pool_private),
				      rte_socket_id(), 0);
	if (!mp)
		return NULL;

	ret = rte_mempool_set_ops_byname(mp, "flux_iokd_completion", NULL);
	if (ret) {
		rte_mempool_free(mp);
		return NULL;
	}

	rte_pktmbuf_pool_init(mp, &mbp_priv);
	ret = rte_mempool_populate_default(mp);
	if (ret < 0) {
		rte_mempool_free(mp);
		return NULL;
	}

	rte_mempool_obj_iter(mp, rte_pktmbuf_init, NULL);
	rte_mempool_obj_iter(mp, flux_iokd_tx_pktmbuf_priv_init, NULL);
	return mp;
}

static int flux_iokd_tx_drain_queue(struct flux_iokd_client *client,
				    int cpu_idx, int n,
				    struct flux_iokd_pkt *pkts)
{
	int i;
	struct flux_iokd_pkt *pkt;
	uintptr_t client_tx_base;
	uintptr_t tx_base;

	client_tx_base = FLUX_MEMORY_ADDR;
	tx_base = client->window_base + FLUX_IOK_DMA_OFFSET;
	for (i = 0; i < n; i++) {
		uintptr_t client_mbuf;
		uintptr_t iokd_mbuf;
		uintptr_t client_data;

		pkt = &pkts[i];
		if (!lrpc_recv(&client->pcpus[cpu_idx].txpktq,
			       &pkt->cmd.lrpc_cmd, &pkt->comp_data))
			break;

		pkt->hash = flux_rss_from_txpkt_payload(pkt->comp_data);
		pkt->comp_data = flux_txpkt_offset_from_payload(pkt->comp_data);
		pkt->dst_ip = pkt->cmd.dst_ip;
		pkt->len = pkt->cmd.len;
		pkt->olflags = pkt->cmd.olflags;
		if (!flux_iokd_client_get(client))
			break;
		pkt->client = client;
		pkt->cpu_idx = cpu_idx;
		client_mbuf = flux_txpkt_ptr_from_payload(pkt->comp_data,
							  client_tx_base);
		iokd_mbuf =
			flux_txpkt_ptr_from_payload(pkt->comp_data, tx_base);
		client_data =
			(uintptr_t)flux_txpkt_mbuf_data_from_ptr(iokd_mbuf);
		if (unlikely(client_data < client_mbuf)) {
			flux_iokd_client_put(client);
			break;
		}
		pkt->buf = (char *)(iokd_mbuf + (client_data - client_mbuf));
	}

	return i;
}

static int flux_iokd_pkt_to_mbuf(struct rte_mbuf *buf,
				 const struct flux_iokd_pkt *pkt)
{
	struct flux_iokd_tx_priv *priv;
	void *dst;

	rte_pktmbuf_reset(buf);
	priv = rte_mbuf_to_priv(buf);
	priv->client = pkt->client;
	priv->cpu_idx = pkt->cpu_idx;
	priv->comp_data = pkt->comp_data;

	if (flux_iokd_cfg.tx_copy) {
		dst = rte_pktmbuf_append(buf, pkt->len);
		if (!dst)
			return -ENOBUFS;
		memcpy(dst, pkt->buf, pkt->len);
	} else {
		buf->buf_addr = pkt->buf;
		buf->buf_iova = rte_mem_virt2iova(pkt->buf);
		buf->data_off = 0;
		rte_mbuf_refcnt_set(buf, 1);
		buf->buf_len = pkt->len;
		buf->pkt_len = pkt->len;
		buf->data_len = pkt->len;
	}

	buf->ol_flags = 0;
	if (pkt->olflags != 0 && flux_iokd.tx_chksum_offload) {
		if (pkt->olflags & FLUX_OLFLAG_IP_CHKSUM)
			buf->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
		if (pkt->olflags & FLUX_OLFLAG_L3_CHKSUM)
			buf->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM |
					 RTE_MBUF_F_TX_UDP_CKSUM;
		if (pkt->olflags & FLUX_OLFLAG_IPV4)
			buf->ol_flags |= RTE_MBUF_F_TX_IPV4;
		if (pkt->olflags & FLUX_OLFLAG_IPV6)
			buf->ol_flags |= RTE_MBUF_F_TX_IPV6;
		buf->l4_len = sizeof(struct rte_tcp_hdr);
		buf->l3_len = sizeof(struct rte_ipv4_hdr);
		buf->l2_len = RTE_ETHER_HDR_LEN;
	}

	return 0;
}

bool flux_iokd_tx_burst(void)
{
	struct rte_mbuf *tmp[FLUX_FNET_TX_BURST_SIZE];
	int budget, i, j, ret;
	bool work_done = false;

	if (!flux_iokd.fnet_has_port)
		return false;

	budget = FLUX_FNET_TX_BURST_SIZE -
		 (flux_iokd_tx_n_pkts + flux_iokd_tx_n_bufs);
	if (budget > 0) {
		for (i = 0; i < FLUX_IOKD_MAX_CLIENTS && budget > 0; i++) {
			struct flux_iokd_client *client =
				flux_iokd_client_load(i);
			if (!client || !flux_iokd_client_ready(client))
				continue;
			for (j = 0; j < client->nr_cpus && budget > 0; j++) {
				int cpu_idx =
					flux_iokd_tx_next_cpu_rr[i]++ %
					client->nr_cpus;

				ret = flux_iokd_tx_drain_queue(
					client, cpu_idx, budget,
					&flux_iokd_tx_pkts[flux_iokd_tx_n_pkts]);
				flux_iokd_tx_n_pkts += ret;
				budget -= ret;
			}
		}
	}

	if (flux_iokd_tx_n_pkts > 0) {
		ret = rte_mempool_get_bulk(flux_iokd.tx_mempool, (void **)tmp,
					   flux_iokd_tx_n_pkts);
		if (ret == 0) {
			for (i = 0; i < flux_iokd_tx_n_pkts; i++) {
				if (flux_iokd_pkt_to_mbuf(
					    tmp[i], &flux_iokd_tx_pkts[i]) == 0)
					flux_iokd_tx_bufs[flux_iokd_tx_n_bufs++] =
						tmp[i];
				else {
					flux_iokd_client_put(
						flux_iokd_tx_pkts[i].client);
					rte_pktmbuf_free(tmp[i]);
				}
			}
			work_done = flux_iokd_tx_n_bufs > 0;
		} else {
			FLUX_LOG(FLUX_LOG_WARN, "tx mempool empty staged=%d\n",
				 flux_iokd_tx_n_pkts);
			for (i = 0; i < flux_iokd_tx_n_pkts; i++)
				flux_iokd_client_put(
					flux_iokd_tx_pkts[i].client);
		}
		flux_iokd_tx_n_pkts = 0;
	}

	if (flux_iokd_tx_n_bufs > 0) {
		ret = rte_eth_tx_burst(FLUX_FNET_PORT, 0, flux_iokd_tx_bufs,
				       flux_iokd_tx_n_bufs);
		if (ret < flux_iokd_tx_n_bufs) {
			for (i = ret; i < flux_iokd_tx_n_bufs; i++)
				flux_iokd_tx_bufs[i - ret] =
					flux_iokd_tx_bufs[i];
		}
		flux_iokd_tx_n_bufs -= ret;
		work_done = true;
	}

	return work_done;
}

void flux_iokd_tx_shutdown_flush(void)
{
	int i;

	for (i = 0; i < flux_iokd_tx_n_pkts; i++)
		flux_iokd_client_put(flux_iokd_tx_pkts[i].client);
	flux_iokd_tx_n_pkts = 0;

	for (i = 0; i < flux_iokd_tx_n_bufs; i++)
		rte_pktmbuf_free(flux_iokd_tx_bufs[i]);
	flux_iokd_tx_n_bufs = 0;
}

bool flux_iokd_drain_completions(void)
{
	struct flux_iokd_client *client;
	union flux_rxq_cmd cmd = { .rxcmd = FLUX_RX_NET_COMP };
	bool work_done = false;
	int i;

	if (!flux_iokd.fnet_has_port)
		return false;

	if (likely(!flux_iokd.tx_comp_pending))
		return false;

	flux_iokd.tx_comp_pending = false;
	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		client = flux_iokd_client_load(i);
		if (!client || !flux_iokd_client_ready(client))
			continue;
		while (client->tx_comp_nr_ofs > 0) {
			unsigned long comp;
			int cpu_idx;

			cpu_idx = client->tx_next_cpu_rr++ % client->nr_cpus;
			comp = client->tx_comp_ofq[client->tx_comp_nr_ofs - 1];
			if (!lrpc_send(&client->pcpus[cpu_idx].rxq,
				       cmd.lrpc_cmd, comp))
				break;
			client->tx_comp_nr_ofs--;
			work_done = true;
		}
		if (client->tx_comp_nr_ofs > 0)
			flux_iokd.tx_comp_pending = true;
	}

	return work_done;
}

bool flux_iokd_commands_rx(void)
{
	struct flux_iokd_client *client;
	union flux_txcmdq_cmd cmd;
	unsigned long payload;
	bool work_done = false;
	int i;
	int j;

	if (!flux_iokd.fnet_has_port)
		return false;

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		client = flux_iokd_client_load(i);
		if (!client || !flux_iokd_client_ready(client))
			continue;
		for (j = 0; j < client->nr_cpus; j++) {
			while (lrpc_recv(&client->pcpus[j].txcmdq,
					 &cmd.lrpc_cmd, &payload)) {
				if (cmd.txcmd != FLUX_TXCMD_NET_COMP)
					continue;
				flux_iokd_rx_mbuf_complete(client, payload);
				work_done = true;
			}
		}
	}

	return work_done;
}
