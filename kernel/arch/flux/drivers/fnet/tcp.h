/* tcp.h - local header for TCP support */
#ifndef _FLUX_IOK_NET_TCP_H
#define _FLUX_IOK_NET_TCP_H

#include <linux/wait.h>
#include <linux/list.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/tcp.h>
#include <uapi/linux/wait.h>
#include <asm/current.h>

#include "net.h"
#include "mbuf.h"
#include "trans.h"
#include "poll.h"
#include "wait.h"
#include "core.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PUSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20
#define TCP_ECE 0x40
#define TCP_CWR 0x80
#define TCP_FLAGS                                                     \
	(TCP_FIN | TCP_SYN | TCP_RST | TCP_PUSH | TCP_ACK | TCP_URG | \
	 TCP_ECE | TCP_CWR)
#define PRINT_TCP_FLAGS "\20\1FIN\2SYN\3RST\4PUSH\5ACK\6URG\7ECE\10CWR"

/* adjustable constants */
#define TCP_WIN 0x7FFFF
#define TCP_ACK_TIMEOUT (10 * USEC_PER_MSEC)
#define TCP_CONNECT_TIMEOUT (5 * USEC_PER_SEC) /* FIXME */
#define TCP_OOQ_ACK_TIMEOUT (300 * USEC_PER_MSEC)
#define TCP_TIME_WAIT_TIMEOUT \
	(1 * USEC_PER_SEC) /* FIXME: should be 8 minutes */
#define TCP_ZERO_WND_TIMEOUT \
	(300 * USEC_PER_MSEC) /* FIXME: should be dynamic */
#define TCP_RETRANSMIT_TIMEOUT \
	(300 * USEC_PER_MSEC) /* FIXME: should be dynamic */
#define TCP_FAST_RETRANSMIT_THRESH 3
#define TCP_OOO_MAX_SIZE 2048
#define TCP_RETRANSMIT_BATCH 16

#define TCPOPT_MSS_ENBIT BIT(0)
#define TCPOPT_WINDOW_ENBIT BIT(1)

/* connecion states (RFC 793 Section 3.2) */
enum {
	TCP_STATE_SYN_SENT = 0,
	TCP_STATE_SYN_RECEIVED,
	TCP_STATE_ESTABLISHED,
	TCP_STATE_FIN_WAIT1,
	TCP_STATE_FIN_WAIT2,
	TCP_STATE_CLOSE_WAIT,
	TCP_STATE_CLOSING,
	TCP_STATE_LAST_ACK,
	TCP_STATE_TIME_WAIT,
	TCP_STATE_CLOSED,
};

/**
 * tcp_calculate_mss - given an ethernet MTU, returns the TCP MSS
 * @mtu: the ethernet mtu
 */
static inline unsigned int tcp_calculate_mss(unsigned int mtu)
{
	return mtu - sizeof(struct iphdr) - sizeof(struct tcphdr);
}

/* TCP protocol control block (PCB) */
struct tcp_pcb {
	int state; /* the connection state */

	/* send sequence variables (RFC 793 Section 3.2) */
	uint32_t snd_una; /* send unacknowledged */
	uint32_t snd_nxt; /* send next */
	uint32_t snd_wnd; /* send window */
	uint32_t snd_up; /* send urgent pointer */
	uint32_t snd_wl1; /* last window update - seq number */
	uint32_t snd_wl2; /* last window update - ack number */
	uint32_t iss; /* initial send sequence number */
	uint32_t snd_wscale; /* the send window scale */
	uint32_t snd_mss; /* the send max segment size */

	/* receive sequence variables (RFC 793 Section 3.2) */
	union {
		struct {
			uint32_t rcv_nxt; /* receive next */
			uint32_t rcv_wnd; /* receive window */
		};
		uint64_t rcv_nxt_wnd;
	};
	uint32_t rcv_up; /* receive urgent pointer */
	uint32_t irs; /* initial receive sequence number */
	uint32_t rcv_wscale; /* the receive window scale */
	uint32_t rcv_mss; /* the send max segment size */
};

/* the TCP connection struct */
struct tcp_conn {
	struct trans_entry e;
	struct tcp_pcb pcb;
	struct list_head global_link;
	uint64_t next_timeout;
	spinlock_t lock;
	struct kref ref;
	int err; /* error code for read(), write(), etc. */
	uint32_t winmax; /* initial receive window size */
	struct poll_head poll;

	/* ingress path */
	bool rx_closed;
	bool rx_exclusive;
	tcp_wait_queue_head_t rx_wq;
	unsigned int rxq_ooo_len;
	struct list_head rxq_ooo;
	struct list_head rxq;

	/* egress path */
	bool tx_closed;
	bool tx_exclusive;
	tcp_wait_queue_head_t tx_wq;
	uint32_t tx_last_ack;
	uint32_t tx_last_win;
	struct mbuf *tx_pending;
	struct list_head txq;
	bool do_fast_retransmit;
	uint32_t fast_retransmit_last_ack;
	struct work_struct retransmit_work;

	/* timeouts */
	uint64_t ack_ts;
	uint64_t zero_wnd_ts;
	union {
		uint64_t time_wait_ts;
		uint64_t attach_ts;
	};
	bool zero_wnd;
	bool ack_delayed;
	int rep_acks;
	int acks_delayed_cnt;

	struct list_head queue_link;
};
typedef struct tcp_conn tcp_conn_t;

extern tcp_conn_t *tcp_conn_alloc(void);
extern int tcp_conn_attach(tcp_conn_t *c, struct net_addr laddr,
			   struct net_addr raddr);
extern void tcp_conn_ack(tcp_conn_t *c, struct list_head *freeq);
extern void tcp_conn_set_state(tcp_conn_t *c, int new_state);
extern void tcp_conn_fail(tcp_conn_t *c, int err);
extern void tcp_conn_shutdown_rx(tcp_conn_t *c);
extern void tcp_conn_destroy(tcp_conn_t *c);
extern void tcp_timer_update(tcp_conn_t *c);
/**
 * tcp_conn_get - increments the connection ref count
 * @c: the connection to increment
 *
 * Returns @c.
 */
static inline tcp_conn_t *tcp_conn_get(tcp_conn_t *c)
{
	kref_get(&c->ref);
	return c;
}

extern void tcp_conn_release_ref(struct kref *r);

/**
 * tcp_conn_put - decrements the connection ref count
 * @c: the connection to decrement
 */
static inline void tcp_conn_put(tcp_conn_t *c)
{
	kref_put(&c->ref, tcp_conn_release_ref);
}

struct tcp_options {
	int opt_en;
	uint16_t mss;
	uint8_t wscale;
};

/*
 * ingress path
 */

extern void tcp_rx_conn(struct trans_entry *e, struct mbuf *m);
extern tcp_conn_t *tcp_rx_listener(struct net_addr laddr, struct mbuf *m);

/*
 * egress path
 */

typedef uint32_t tcp_seq;

extern int tcp_tx_raw_rst(struct net_addr laddr, struct net_addr raddr,
			  tcp_seq seq);
extern int tcp_tx_raw_rst_ack(struct net_addr laddr, struct net_addr raddr,
			      tcp_seq seq, tcp_seq ack);
extern int tcp_tx_ack(tcp_conn_t *c);
extern int tcp_tx_probe_window(tcp_conn_t *c);
extern int tcp_tx_ctl(tcp_conn_t *c, uint8_t flags,
		      const struct tcp_options *opts);
extern ssize_t tcp_tx_send(tcp_conn_t *c, const void *buf, size_t len,
			   bool push);
extern void tcp_tx_retransmit(tcp_conn_t *c);
extern struct mbuf *tcp_tx_fast_retransmit_start(tcp_conn_t *c);
extern void tcp_tx_fast_retransmit_finish(tcp_conn_t *c, struct mbuf *m);

/*
 * utilities
 */

/* free all mbufs in a linked list */
static inline void mbuf_list_free(struct list_head *h)
{
	struct mbuf *m;

	while (true) {
		m = list_first_entry_or_null(h, struct mbuf, link);
		if (!m)
			break;
		list_del(&m->link);
		mbuf_free(m);
	}
}

/* is the TX window full? */
static inline bool tcp_is_snd_full(tcp_conn_t *c)
{
	return wraps_lte(c->pcb.snd_una + c->pcb.snd_wnd, c->pcb.snd_nxt);
}

#endif /* _FLUX_IOK_NET_TCP_H */
