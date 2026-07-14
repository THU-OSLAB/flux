#ifndef _FLUX_IOK_NET_H
#define _FLUX_IOK_NET_H

#include <linux/types.h>

#include "poll.h"

/* TCP operations */

struct tcp_accq;
typedef struct tcp_accq tcp_accq_t;
struct tcp_conn;
typedef struct tcp_conn tcp_conn_t;

struct iovec;

struct net_addr {
	uint32_t ip;
	uint16_t port;
};

extern int flux_tcp_dial(struct net_addr laddr, struct net_addr raddr,
			 tcp_conn_t **c_out);
extern int flux_tcp_dial_nonblocking(struct net_addr laddr,
				     struct net_addr raddr, tcp_conn_t **c_out);
extern int flux_tcp_dial_affinity(uint32_t affinity, struct net_addr raddr,
				  tcp_conn_t **c_out);
extern int flux_tcp_dial_conn_affinity(tcp_conn_t *in, struct net_addr raddr,
				       tcp_conn_t **c_out);

extern int flux_tcp_listen(struct net_addr laddr, int backlog,
			   tcp_accq_t **q_out);
extern int flux_tcp_accept(tcp_accq_t *q, bool nonblocking,
			   tcp_conn_t **c_out);
extern void flux_tcp_accq_shutdown(tcp_accq_t *q);
extern void flux_tcp_accq_close(tcp_accq_t *q);
extern struct net_addr flux_tcp_accq_local_addr(tcp_accq_t *q);
extern int flux_tcp_accq_backlog(tcp_accq_t *q);
extern struct net_addr flux_tcp_local_addr(tcp_conn_t *c);
extern struct net_addr flux_tcp_remote_addr(tcp_conn_t *c);
extern int flux_tcp_get_status(tcp_conn_t *c);
extern uint32_t flux_tcp_get_input_bytes(tcp_conn_t *c);

extern ssize_t flux_tcp_read2(tcp_conn_t *c, void *buf, size_t len, bool peek,
			      bool nonblocking);
extern ssize_t flux_tcp_write2(tcp_conn_t *c, const void *buf, size_t len,
			       bool nonblocking);
extern ssize_t flux_tcp_readv2(tcp_conn_t *c, const struct iovec *iov,
			       int iovcnt, bool peek, bool nonblocking);
extern ssize_t flux_tcp_writev2(tcp_conn_t *c, const struct iovec *iov,
				int iovcnt, bool nonblocking);

static inline ssize_t flux_tcp_read(tcp_conn_t *c, void *buf, size_t len)
{
	return flux_tcp_read2(c, buf, len, false, false);
}

static inline ssize_t flux_tcp_write(tcp_conn_t *c, const void *buf, size_t len)
{
	return flux_tcp_write2(c, buf, len, false);
}

static inline ssize_t flux_tcp_readv(tcp_conn_t *c, const struct iovec *iov,
				     int iovcnt)
{
	return flux_tcp_readv2(c, iov, iovcnt, false, false);
}

static inline ssize_t flux_tcp_writev(tcp_conn_t *c, const struct iovec *iov,
				      int iovcnt)
{
	return flux_tcp_writev2(c, iov, iovcnt, false);
}

extern int flux_tcp_shutdown(tcp_conn_t *c, int how);
extern void flux_tcp_abort(tcp_conn_t *c);
extern void flux_tcp_close(tcp_conn_t *c);

extern void flux_tcp_poll_install_cb(tcp_conn_t *c, poll_notif_fn_t setfn,
				     poll_notif_fn_t clearfn,
				     unsigned long data);
extern void flux_tcp_accq_poll_install_cb(tcp_accq_t *q, poll_notif_fn_t setfn,
					  poll_notif_fn_t clearfn,
					  unsigned long data);

extern int flux_tcp_init(void);

/* transport operations */

extern int flux_trans_init(void);

/* rx handlers */

union flux_rxq_cmd;
extern bool flux_fast_net_rx_recv(unsigned long payload, union flux_rxq_cmd cmd);

#endif /* _FLUX_IOK_NET_H */
