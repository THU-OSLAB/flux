/*
 * transport.c - handles transport protocol packets (UDP and TCP)
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <net/ip.h>

#include "fnet.h"
#include "core.h"
#include "trans.h"

#define TRANS_TBL_SIZE 16384

/* ephemeral port definitions (IANA suggested range) */
#define MIN_EPHEMERAL 49152
#define MAX_EPHEMERAL 65535

/* a seed value for transport handler table hashing calculations */
static uint32_t trans_seed;

/* a simple counter used to further randomize ephemeral ports */
static uint32_t ephemeral_offset;

static inline uint32_t trans_hash_3tuple(uint8_t proto, struct net_addr laddr)
{
	return hash_crc32c_one(
		trans_seed, (uint64_t)laddr.ip | ((uint64_t)laddr.port << 32) |
				    ((uint64_t)proto << 48));
}

static inline uint32_t trans_hash_5tuple(uint8_t proto, struct net_addr laddr,
					 struct net_addr raddr)
{
	return hash_crc32c_two(
		trans_seed, (uint64_t)laddr.ip | ((uint64_t)laddr.port << 32),
		(uint64_t)raddr.ip | ((uint64_t)raddr.port << 32) |
			((uint64_t)proto << 48));
}

static DEFINE_SPINLOCK(trans_lock);
static struct hlist_head trans_tbl[TRANS_TBL_SIZE];

/**
 * trans_table_add - adds an entry to the match table
 * @e: the entry to add
 *
 * Returns 0 if successful, or -EADDRINUSE if a conflicting entry is already in
 * the table, or -EINVAL if the local port is zero.
 */
int trans_table_add(struct trans_entry *e)
{
	struct trans_entry *pos;
	uint32_t idx;

	/* port zero is reserved for ephemeral port auto-assign */
	if (e->laddr.port == 0)
		return -EINVAL;

	pr_debug("add proto %u laddr %pI4:%u raddr %pI4:%u\n", e->proto,
		 &e->laddr.ip, e->laddr.port, &e->raddr.ip, e->raddr.port);

	if (e->match == TRANS_MATCH_3TUPLE)
		idx = trans_hash_3tuple(e->proto, e->laddr);
	else
		idx = trans_hash_5tuple(e->proto, e->laddr, e->raddr);
	idx %= TRANS_TBL_SIZE;

	spin_lock(&trans_lock);
	hlist_for_each_entry_rcu(pos, &trans_tbl[idx], link, true) {
		if (pos->match != e->match)
			continue;
		if (e->match == TRANS_MATCH_3TUPLE && e->proto == pos->proto &&
		    e->laddr.ip == pos->laddr.ip &&
		    e->laddr.port == pos->laddr.port) {
			spin_unlock(&trans_lock);
			return -EADDRINUSE;
		} else if (e->proto == pos->proto &&
			   e->laddr.ip == pos->laddr.ip &&
			   e->laddr.port == pos->laddr.port &&
			   e->raddr.ip == pos->raddr.ip &&
			   e->raddr.port == pos->raddr.port) {
			spin_unlock(&trans_lock);
			return -EADDRINUSE;
		}
	}
	hlist_add_head_rcu(&e->link, &trans_tbl[idx]);
	store_release(&ephemeral_offset, ephemeral_offset + 1);
	spin_unlock(&trans_lock);

	return 0;
}

/**
 * trans_table_add_with_ephemeral_port - adds an entry to the match table
 * while automatically selecting the local port number
 * @e: the entry to add
 *
 * We use algorithm 3 from RFC 6056.
 *
 * Returns 0 if successful or -EADDRNOTAVAIL if all ports are taken.
 */
int trans_table_add_with_ephemeral_port(struct trans_entry *e)
{
	uint16_t offset, next_ephemeral = 0;
	uint16_t num_ephemeral = MAX_EPHEMERAL - MIN_EPHEMERAL + 1;
	int ret;

	e->laddr.port = 0;
	if (e->match == TRANS_MATCH_3TUPLE) {
		offset = trans_hash_3tuple(e->proto, e->laddr) +
			 load_acquire(&ephemeral_offset);
	} else {
		offset = trans_hash_5tuple(e->proto, e->laddr, e->raddr) +
			 load_acquire(&ephemeral_offset);
	}

	while (next_ephemeral < num_ephemeral) {
		uint32_t port = MIN_EPHEMERAL +
				(next_ephemeral++ + offset) % num_ephemeral;
		e->laddr.port = port;
		ret = trans_table_add(e);
		if (!ret)
			return 0;
	}

	return -EADDRNOTAVAIL;
}

/**
 * trans_table_remove - removes an entry from the match table
 * @e: the entry to remove
 *
 * The caller is responsible for eventually freeing the object with rcu_free().
 */
void trans_table_remove(struct trans_entry *e)
{
	spin_lock(&trans_lock);
	hlist_del_rcu(&e->link);
	spin_unlock(&trans_lock);
}

/* the first 4 bytes are identical for TCP and UDP */
struct l4_hdr {
	uint16_t sport, dport;
};

static struct trans_entry *trans_lookup(struct mbuf *m, bool reverse)
{
	const struct iphdr *iphdr;
	const struct l4_hdr *l4hdr;
	struct trans_entry *e;
	struct net_addr laddr, raddr;
	uint32_t hash;

	iphdr = mbuf_network_hdr(m, *iphdr);
	if (unlikely(iphdr->protocol != IPPROTO_UDP &&
		     iphdr->protocol != IPPROTO_TCP))
		return NULL;
	l4hdr = (struct l4_hdr *)mbuf_data(m);
	if (unlikely(mbuf_length(m) < sizeof(*l4hdr)))
		return NULL;

	/* parse the source and destination network address */
	laddr.ip = ntohl(iphdr->daddr);
	laddr.port = ntohs(l4hdr->dport);
	raddr.ip = ntohl(iphdr->saddr);
	raddr.port = ntohs(l4hdr->sport);

	if (unlikely(reverse))
		swap(laddr, raddr);

	rcu_read_lock();

	/* attempt to find a 5-tuple match */
	hash = trans_hash_5tuple(iphdr->protocol, laddr, raddr);
	hlist_for_each_entry_rcu(e, &trans_tbl[hash % TRANS_TBL_SIZE], link,
				 false) {
		if (e->match != TRANS_MATCH_5TUPLE)
			continue;
		if (e->proto == iphdr->protocol && e->laddr.ip == laddr.ip &&
		    e->laddr.port == laddr.port && e->raddr.ip == raddr.ip &&
		    e->raddr.port == raddr.port) {
			rcu_read_unlock();
			return e;
		}
	}

	/* attempt to find a 3-tuple match */
	hash = trans_hash_3tuple(iphdr->protocol, laddr);
	hlist_for_each_entry_rcu(e, &trans_tbl[hash % TRANS_TBL_SIZE], link,
				 false) {
		if (e->match != TRANS_MATCH_3TUPLE)
			continue;
		if (e->proto == iphdr->protocol && e->laddr.ip == laddr.ip &&
		    e->laddr.port == laddr.port) {
			rcu_read_unlock();
			return e;
		}
	}

	rcu_read_unlock();
	return NULL;
}

/**
 * net_rx_trans - receive an L4 packet
 * m: the mbuf to receive
 */
bool net_rx_trans_match(struct mbuf *m)
{
	struct trans_entry *e;

	mbuf_mark_transport_offset(m);

	rcu_read_lock();
	e = trans_lookup(m, false);
	if (unlikely(!e)) {
		rcu_read_unlock();
		return false;
	}

	e->ops->recv(e, m);
	rcu_read_unlock();
	return true;
}

void net_rx_trans(struct mbuf *m)
{
	const struct iphdr *iphdr;

	if (net_rx_trans_match(m))
		return;

	iphdr = mbuf_network_hdr(m, *iphdr);
	if (iphdr->protocol == IPPROTO_TCP)
		tcp_rx_closed(m);
	mbuf_free(m);
}

/**
 * trans_error - reports a network error to the L4 layer
 * @m: the mbuf that triggered the error
 * @err: the suggested ernno to report
 */
void trans_error(struct mbuf *m, int err)
{
	struct trans_entry *e;

	rcu_read_lock();
	e = trans_lookup(m, true);
	if (e && e->ops->err)
		e->ops->err(e, err);
	rcu_read_unlock();
}

/**
 * trans_init - initializes transport protocol infrastructure
 *
 * Returns 0 (always successful).
 */
int flux_trans_init(void)
{
	int i;

	spin_lock_init(&trans_lock);

	for (i = 0; i < TRANS_TBL_SIZE; i++)
		INIT_HLIST_HEAD(&trans_tbl[i]);

	trans_seed =
		rand_crc32c(0x48FA8BC1 ^ rand_crc32c(fnet_dev->fast_net_ip));
	return 0;
}
