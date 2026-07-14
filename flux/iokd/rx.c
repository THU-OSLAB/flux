#define FLUX_FMT "iokd-rx: "

#include <rte_arp.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_ip.h>
#include <rte_jhash.h>
#include <rte_mempool.h>

#include <kernel/asm/fnet.h>
#include <utils/log.h>

#include "dpdk_errno.h"
#include "iokd.h"

#define FLUX_IOKD_CLIENT_IP_HASH_NAME "FLUX_IOKD_CLIENT_IP"
#define FLUX_IOKD_CLIENT_IP_HASH_ENTRIES 8

static unsigned long flux_iokd_rx_data_to_offset(struct rte_mbuf *buf)
{
	return (unsigned long)((uintptr_t)rte_pktmbuf_mtod(buf, void *) -
			       (uintptr_t)flux_iokd.ctrl.rx_shbuf);
}

static struct flux_iokd_client *flux_iokd_client_lookup_ip(uint32_t dst_ip)
{
	struct flux_iokd_client *client = NULL;
	void *data = NULL;
	int ret;

	if (!flux_iokd.client_ip_hash)
		return NULL;

	ret = rte_hash_lookup_data(flux_iokd.client_ip_hash, &dst_ip, &data);
	if (ret < 0)
		return NULL;

	client = data;
	if (!client || !flux_iokd_client_ready(client))
		return NULL;

	return client;
}

int flux_iokd_rx_clients_init(void)
{
	struct rte_hash_parameters params = {
		.name = FLUX_IOKD_CLIENT_IP_HASH_NAME,
		.entries = FLUX_IOKD_CLIENT_IP_HASH_ENTRIES,
		.key_len = sizeof(uint32_t),
		.hash_func = rte_jhash,
		.socket_id = rte_socket_id(),
		.extra_flag = RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD |
			      RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY,
	};

	flux_iokd.client_ip_hash = rte_hash_create(&params);
	if (!flux_iokd.client_ip_hash) {
		FLUX_LOG(FLUX_LOG_ERR, "rte_hash_create failed: %s\n",
			 rte_strerror(rte_errno));
		return -rte_errno;
	}

	return 0;
}

int flux_iokd_rx_client_register(struct flux_iokd_client *client)
{
	uint32_t ip;
	int ret;

	if (!flux_iokd.client_ip_hash || !client || !client->ip_addr)
		return 0;

	ip = client->ip_addr;
	ret = rte_hash_lookup(flux_iokd.client_ip_hash, &ip);
	if (ret != -ENOENT) {
		FLUX_LOG(FLUX_LOG_ERR, "duplicate client ip %#x client=%d\n", ip,
			 client->id);
		return ret >= 0 ? -EEXIST : ret;
	}

	ret = rte_hash_add_key_data(flux_iokd.client_ip_hash, &ip, client);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rte_hash_add_key_data failed client=%d ip=%#x: %s\n",
			 client->id, ip, rte_strerror(-ret));
		return ret;
	}

	return 0;
}

void flux_iokd_rx_client_queue_register(struct flux_iokd_client *client)
{
	struct flux_iokd_client *head;

	if (!client)
		return;

	do {
		head = atomic_load_relaxed(&flux_iokd.pending_clients);
		client->pending_reg_next = head;
	} while (!atomic_cmpxchg_release_relaxed(&flux_iokd.pending_clients,
						 &head, client));
	flux_iokd_control_wake();
}

void flux_iokd_rx_client_queue_unregister(struct flux_iokd_client *client)
{
	struct flux_iokd_client *head;

	if (!client)
		return;

	do {
		head = atomic_load_relaxed(&flux_iokd.pending_unregister_clients);
		client->pending_unreg_next = head;
	} while (!atomic_cmpxchg_release_relaxed(
		&flux_iokd.pending_unregister_clients, &head, client));
	flux_iokd_control_wake();
}

void flux_iokd_rx_process_pending_clients(void)
{
	struct flux_iokd_client *client;
	struct flux_iokd_client *next;

	client = __atomic_exchange_n(&flux_iokd.pending_unregister_clients, NULL,
				     __ATOMIC_ACQUIRE);
	while (client) {
		next = client->pending_unreg_next;
		client->pending_unreg_next = NULL;
		flux_iokd_rx_client_unregister(client);
		client = next;
	}

	client = __atomic_exchange_n(&flux_iokd.pending_clients, NULL,
				     __ATOMIC_ACQUIRE);
	while (client) {
		next = client->pending_reg_next;
		client->pending_reg_next = NULL;

		if (client->id >= 0 && client->id < FLUX_IOKD_MAX_CLIENTS &&
		    flux_iokd_client_load(client->id) == client &&
		    flux_iokd_client_ready(client))
			flux_iokd_rx_client_register(client);

		client = next;
	}
}

void flux_iokd_rx_client_unregister(struct flux_iokd_client *client)
{
	uint32_t ip;
	int ret;

	if (!flux_iokd.client_ip_hash || !client || !client->ip_addr)
		return;

	ip = client->ip_addr;
	ret = rte_hash_del_key(flux_iokd.client_ip_hash, &ip);
	if (ret < 0 && ret != -ENOENT) {
		FLUX_LOG(FLUX_LOG_WARN,
			 "rte_hash_del_key failed client=%d ip=%#x: %s\n",
			 client->id, ip, rte_strerror(-ret));
	}
}

void flux_iokd_rx_clients_fini(void)
{
	if (!flux_iokd.client_ip_hash)
		return;

	rte_hash_free(flux_iokd.client_ip_hash);
	flux_iokd.client_ip_hash = NULL;
}

static struct rte_mbuf *flux_iokd_rx_mbuf_from_offset(unsigned long payload)
{
	uintptr_t off;
	uintptr_t data_addr;
	uintptr_t page_base;
	size_t slot_idx;
	size_t obj_size;
	size_t objs_per_page;

	off = flux_rxq_payload_to_rx_offset(payload);
	if (unlikely(off >= flux_iokd.ctrl.rx_shbuf_len))
		return NULL;
	data_addr = (uintptr_t)flux_iokd.ctrl.rx_shbuf + off;
	page_base = PGADDR_2MB(data_addr);
	obj_size = flux_iokd.ctrl.rx_obj_size;
	objs_per_page = PGSIZE_2MB / obj_size;
	slot_idx = PGOFF_2MB(data_addr) / obj_size;

	if (unlikely(slot_idx >= objs_per_page))
		return NULL;

	return (struct rte_mbuf *)(page_base + slot_idx * obj_size +
				   flux_iokd.ctrl.rx_obj_off);
}

static void flux_iokd_rx_mbuf_track(struct flux_iokd_client *client,
				    struct rte_mbuf *buf)
{
	struct rx_priv_data *priv = rte_mbuf_to_priv(buf);

	priv->owner = client;
	priv->prev_owned = NULL;
	priv->next_owned = client->owned_rx_bufs;
	if (priv->next_owned)
		((struct rx_priv_data *)rte_mbuf_to_priv(priv->next_owned))
			->prev_owned = buf;
	client->owned_rx_bufs = buf;
}

static bool flux_iokd_rx_mbuf_untrack(struct flux_iokd_client *client,
				      struct rte_mbuf *buf)
{
	struct rx_priv_data *priv = rte_mbuf_to_priv(buf);
	struct rx_priv_data *other;

	if (priv->owner != client)
		return false;

	if (priv->prev_owned) {
		other = rte_mbuf_to_priv(priv->prev_owned);
		other->next_owned = priv->next_owned;
	} else {
		client->owned_rx_bufs = priv->next_owned;
	}

	if (priv->next_owned) {
		other = rte_mbuf_to_priv(priv->next_owned);
		other->prev_owned = priv->prev_owned;
	}

	priv->owner = NULL;
	priv->next_owned = NULL;
	priv->prev_owned = NULL;
	return true;
}

bool flux_iokd_rx_mbuf_deliver(struct flux_iokd_client *client, int cpu_idx,
			       struct rte_mbuf *buf)
{
	union flux_rxq_cmd cmd = {
		.rxcmd = FLUX_RX_NET_RECV,
		.len = rte_pktmbuf_pkt_len(buf),
		.csum_type =
			((buf->ol_flags & FLUX_IOK_MBUF_F_RX_IP_CKSUM_MASK) ==
			 FLUX_IOK_MBUF_F_RX_IP_CKSUM_GOOD) ?
				FLUX_CHKSUM_TYPE_UNNECESSARY :
				FLUX_CHKSUM_TYPE_NEEDED,
	};
	unsigned long payload;

	flux_iokd_rx_mbuf_track(client, buf);
	payload = flux_iokd_rx_data_to_offset(buf);

#ifdef CONFIG_FLUX_FAST_NET
	{
		struct rte_ether_hdr *eth;
		struct rte_ipv4_hdr *ip;

		eth = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
		if (rte_pktmbuf_pkt_len(buf) >= sizeof(*eth) + sizeof(*ip) &&
		    eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
			ip = (struct rte_ipv4_hdr *)(eth + 1);
			if (ip->next_proto_id == IPPROTO_TCP)
				payload =
					flux_rxq_payload_from_fast_net_rx_offset(
						payload);
		}
	}
#endif

	if (!lrpc_send(&client->pcpus[cpu_idx].rxq, cmd.lrpc_cmd, payload)) {
		FLUX_LOG(
			FLUX_LOG_WARN,
			"deliver rx failed client=%d cpu=%d payload=%#lx len=%u\n",
			client->id, cpu_idx, payload, rte_pktmbuf_pkt_len(buf));
		flux_iokd_rx_mbuf_untrack(client, buf);
		return false;
	}

	return true;
}

void flux_iokd_rx_mbuf_complete(struct flux_iokd_client *client,
				unsigned long payload)
{
	struct rte_mbuf *buf;

	buf = flux_iokd_rx_mbuf_from_offset(payload);
	if (!buf)
		return;
	if (!flux_iokd_rx_mbuf_untrack(client, buf))
		return;

	buf = rte_pktmbuf_prefree_seg(buf);
	if (!buf)
		return;

	rte_mbuf_raw_free(buf);
}

void flux_iokd_rx_mbuf_reclaim_all(struct flux_iokd_client *client)
{
	struct rte_mbuf *buf;
	struct rte_mbuf *next;
	struct rx_priv_data *priv;

	for (buf = client->owned_rx_bufs; buf; buf = next) {
		priv = rte_mbuf_to_priv(buf);
		next = priv->next_owned;
		priv->owner = NULL;
		priv->next_owned = NULL;
		priv->prev_owned = NULL;
		rte_pktmbuf_free(buf);
	}

	client->owned_rx_bufs = NULL;
}

static void flux_iokd_rx_one(struct rte_mbuf *buf)
{
	struct flux_iokd_client *client = NULL;
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	struct rte_arp_hdr *arp;
	uint32_t dst_ip = 0;
	int cpu_idx;

	eth = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
		ip = (struct rte_ipv4_hdr *)(eth + 1);
		dst_ip = rte_be_to_cpu_32(ip->dst_addr);
	} else if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
		arp = (struct rte_arp_hdr *)(eth + 1);
		dst_ip = rte_be_to_cpu_32(arp->arp_data.arp_tip);
	}

	if (!dst_ip)
		goto out_free;

	client = flux_iokd_client_lookup_ip(dst_ip);
	if (!client) {
		FLUX_LOG(FLUX_LOG_WARN, "rx no client for dst_ip=%#x\n",
			 dst_ip);
		goto out_free;
	}

	cpu_idx = client->nr_cpus > 0 ?
			  (int)(buf->hash.rss % (uint32_t)client->nr_cpus) :
			  0;
	if (!flux_iokd_rx_mbuf_deliver(client, cpu_idx, buf))
		goto out_free;

	return;

out_free:
	rte_pktmbuf_free(buf);
}

bool flux_iokd_rx_burst(void)
{
	struct rte_mbuf *bufs[FLUX_FNET_RX_BURST_SIZE];
	uint16_t nb_rx;
	uint16_t i;

	if (!flux_iokd.fnet_has_port)
		return false;

	nb_rx = rte_eth_rx_burst(FLUX_FNET_PORT, 0, bufs,
				 FLUX_FNET_RX_BURST_SIZE);
	if (!nb_rx)
		return false;

	for (i = 0; i < nb_rx; i++) {
		if (i + FLUX_IOKD_RX_PREFETCH_STRIDE < nb_rx)
			prefetch(rte_pktmbuf_mtod(
				bufs[i + FLUX_IOKD_RX_PREFETCH_STRIDE],
				char *));
		flux_iokd_rx_one(bufs[i]);
	}

	return true;
}
