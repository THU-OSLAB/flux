#ifndef _FLUX_IOK_TRANS_H
#define _FLUX_IOK_TRANS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rculist.h>

#include "net.h"

enum {
	/* match on protocol, source IP and port */
	TRANS_MATCH_3TUPLE = 0,
	/* match on protocol, source IP and port + dest IP and port */
	TRANS_MATCH_5TUPLE,
};

struct mbuf;
struct trans_entry;

struct trans_ops {
	/* receive an ingress packet */
	void (*recv)(struct trans_entry *e, struct mbuf *m);
	/* propagate a network error */
	void (*err)(struct trans_entry *e, int err);
};

struct trans_entry {
	int match;
	uint8_t proto;
	struct net_addr laddr;
	struct net_addr raddr;
	struct hlist_node link;
	struct rcu_head rcu;
	const struct trans_ops *ops;
};

/**
 * trans_init_3tuple - initializes a transport layer entry (3-tuple match)
 * @e: the entry to initialize
 * @proto: the IP protocol
 * @ops: operations to handle matching flows
 * @laddr: the local address
 */
static inline void trans_init_3tuple(struct trans_entry *e, uint8_t proto,
				     const struct trans_ops *ops,
				     struct net_addr laddr)
{
	e->match = TRANS_MATCH_3TUPLE;
	e->proto = proto;
	e->laddr = laddr;
	e->ops = ops;
	memset(&e->raddr, 0, sizeof(e->raddr));
}

/**
 * trans_init_5tuple - initializes a transport layer entry (5-tuple match)
 * @e: the entry to initialize
 * @proto: the IP protocol
 * @ops: operations to handle matching flows
 * @laddr: the local address
 * @raddr: the remote address
 */
static inline void trans_init_5tuple(struct trans_entry *e, uint8_t proto,
				     const struct trans_ops *ops,
				     struct net_addr laddr, struct net_addr raddr)
{
	e->match = TRANS_MATCH_5TUPLE;
	e->proto = proto;
	e->laddr = laddr;
	e->raddr = raddr;
	e->ops = ops;
}

extern int trans_table_add(struct trans_entry *e);
extern int trans_table_add_with_ephemeral_port(struct trans_entry *e);
extern void trans_table_remove(struct trans_entry *e);

extern void trans_error(struct mbuf *m, int err);

#endif
