/*
 * tcp.c - support for Transmission Control Protocol (RFC 793)
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <linux/in.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/poll.h>

#include "fnet.h"
#include "core.h"
#include "tcp.h"
#include "wait.h"

/* protects @tcp_conns */
static DEFINE_SPINLOCK(tcp_lock);
/* a list of all TCP connections */
static LIST_HEAD(tcp_conns);

static struct task_struct *tcp_worker_blocked;

static void tcp_retransmit(struct work_struct *w);

uint32_t flux_tcp_get_input_bytes(tcp_conn_t *c)
{
	uint32_t b;
	spin_lock(&c->lock);
	b = c->winmax - c->pcb.rcv_wnd;
	spin_unlock(&c->lock);
	return b;
}

void tcp_timer_update(tcp_conn_t *c)
{
	uint64_t next_timeout = -1L;
	struct mbuf *m;

	if (unlikely(c->pcb.state == TCP_STATE_TIME_WAIT))
		next_timeout = c->time_wait_ts + TCP_TIME_WAIT_TIMEOUT;

	if (unlikely(c->pcb.state < TCP_STATE_ESTABLISHED))
		next_timeout = c->attach_ts + TCP_CONNECT_TIMEOUT;

	if (c->ack_delayed)
		next_timeout = min(next_timeout, c->ack_ts + TCP_ACK_TIMEOUT);
	if (c->zero_wnd)
		next_timeout = min(next_timeout,
				   c->zero_wnd_ts + TCP_ZERO_WND_TIMEOUT);

	if (!c->tx_exclusive) {
		m = list_first_entry_or_null(&c->txq, struct mbuf, link);
		if (m)
			next_timeout =
				min(next_timeout,
				    m->timestamp + TCP_RETRANSMIT_TIMEOUT);
	}

	if (!list_empty(&c->rxq_ooo))
		next_timeout =
			min(next_timeout, now_us() + TCP_OOQ_ACK_TIMEOUT);

	store_release(&c->next_timeout, next_timeout);
}

/* check for timeouts in a TCP connection */
static void tcp_handle_timeouts(tcp_conn_t *c, uint64_t now)
{
	bool do_ack = false, do_probe = false, do_retransmit = false;
	struct mbuf *m;

	spin_lock(&c->lock);
	if (unlikely(c->pcb.state == TCP_STATE_CLOSED)) {
		spin_unlock(&c->lock);
		return;
	}

	if (unlikely(c->pcb.state < TCP_STATE_ESTABLISHED) &&
	    now - c->attach_ts >= TCP_CONNECT_TIMEOUT) {
		fnet_dbg("%llx connect timeout", (u64)c);
		tcp_conn_fail(c, ETIMEDOUT);
		spin_unlock(&c->lock);
		return;
	}

	if (c->pcb.state == TCP_STATE_TIME_WAIT &&
	    now - c->time_wait_ts >= TCP_TIME_WAIT_TIMEOUT) {
		fnet_dbg("%llx time wait timeout", (u64)c);
		tcp_conn_set_state(c, TCP_STATE_CLOSED);
		spin_unlock(&c->lock);
		tcp_conn_put(c);
		return;
	}

	if (c->ack_delayed && now - c->ack_ts >= TCP_ACK_TIMEOUT) {
		fnet_dbg("%llx delayed ack timeout", (u64)c);
		c->ack_delayed = false;
		do_ack = true;
	}

	if (c->zero_wnd && now - c->zero_wnd_ts >= TCP_ZERO_WND_TIMEOUT) {
		fnet_dbg("%llx zero window timeout", (u64)c);
		c->zero_wnd_ts = now;
		do_probe = true;
	}

	if (!c->tx_exclusive && !list_empty(&c->txq)) {
		m = list_first_entry(&c->txq, struct mbuf, link);
		if (now - m->timestamp >= TCP_RETRANSMIT_TIMEOUT) {
			fnet_dbg("%llx needs retransmission", (u64)m);
			/* It is safe to take a reference, since state != closed */
			tcp_conn_get(c);
			do_retransmit = true;
		}
	}

	do_ack |= !list_empty(&c->rxq_ooo);

	tcp_timer_update(c);

	spin_unlock(&c->lock);

	if (do_ack)
		tcp_tx_ack(c);
	if (do_probe)
		tcp_tx_probe_window(c);
	if (do_retransmit)
		schedule_work(&c->retransmit_work);
}

/* a periodic background thread that handles timeout events */
static int tcp_worker(void *arg)
{
	tcp_conn_t *c, *tmp;
	uint64_t now;

	flux_may_change_sched_class(current);

	pr_info("timeout worker started\n");

	while (1) {
		spin_lock(&tcp_lock);

		if (unlikely(list_empty(&tcp_conns))) {
			tcp_worker_blocked = current;
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock(&tcp_lock);
			schedule();
			__set_current_state(TASK_RUNNING);
			continue;
		}

		list_for_each_entry_safe(c, tmp, &tcp_conns, global_link) {
			now = now_us();
			if (load_acquire(&c->next_timeout) <= now)
				tcp_handle_timeouts(c, now);
		}
		spin_unlock(&tcp_lock);
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	}

	return 0;
}

/**
 * tcp_free_rx_bufs - try to free some buffers when we are running low
 *
 * This function scans all active TCP connections, dropping packets in the
 * out-of-order queue and waking any waiters to handle in-order packets
 */
void tcp_free_rx_bufs(void)
{
	tcp_conn_t *c;

	LIST_HEAD(mbufs);
	LIST_HEAD(waiters);

	spin_lock(&tcp_lock);

	list_for_each_entry(c, &tcp_conns, global_link) {
		if (list_empty(&c->rxq_ooo) && list_empty(&c->rxq))
			continue;

		spin_lock(&c->lock);
		tcp_wait_queue_wake_up_all_start(&c->rx_wq, &waiters);
		list_splice_init(&c->rxq_ooo, &mbufs);
		c->rxq_ooo_len = 0;
		spin_unlock(&c->lock);
	}

	spin_unlock(&tcp_lock);

	mbuf_list_free(&mbufs);
	tcp_wait_queue_wake_up_all_finish(&waiters);
}

/**
 * tcp_conn_ack - removes acknowledged packets from TX queue
 *
 * @c: the TCP connection to update
 * @freeq: a pointer to a list to store acknowledged buffers to later free
 *
 * WARNING: the caller must hold @c->lock.
 * @freeq is provided so that @c->lock can be released before freeing buffers.
 */
void tcp_conn_ack(tcp_conn_t *c, struct list_head *freeq)
{
	struct mbuf *m;

	/* will free these segments later */
	if (c->tx_exclusive)
		return;

	/* dequeue buffers that are fully acknowledged */
	while (1) {
		m = list_first_entry_or_null(&c->txq, struct mbuf, link);
		if (!m)
			break;

		if (wraps_gt(m->seg_end, c->pcb.snd_una))
			break;

		fnet_dbg("acknowledged seg 0x%llx\n", (u64)m);

		list_del(&m->link);
		list_add_tail(&m->link, freeq);
	}
}

/**
 * tcp_conn_set_state - changes the TCP PCB state
 * @c: the TCP connection to update
 * @new_state: the new TCP_STATE_* value
 *
 * WARNING: @c->lock must be held by the caller.
 * WARNING: @new_state must be greater than the current state.
 */
void tcp_conn_set_state(tcp_conn_t *c, int new_state)
{
	/* unblock any threads waiting for the connection to be established */
	if (c->pcb.state < TCP_STATE_ESTABLISHED &&
	    new_state >= TCP_STATE_ESTABLISHED) {
		tcp_wait_queue_wake_up_all(&c->tx_wq);
		poll_set(&c->poll, POLLOUT);
	}

	c->pcb.state = new_state;
	tcp_timer_update(c);
}

/* handles network errors for TCP sockets */
static void tcp_conn_err(struct trans_entry *e, int err)
{
	tcp_conn_t *c = container_of(e, tcp_conn_t, e);

	spin_lock(&c->lock);
	tcp_conn_fail(c, err);
	spin_unlock(&c->lock);
}

/* operations for TCP sockets */
static const struct trans_ops tcp_conn_ops = {
	.recv = tcp_rx_conn,
	.err = tcp_conn_err,
};

/*
 * Connection initialization
 */

static inline uint32_t tcp_scale_window(uint32_t maxwin)
{
	uint32_t wscale = 0;

	while (maxwin > 0xffff && wscale < 14) {
		maxwin >>= 1;
		wscale++;
	}

	return wscale;
}

int flux_tcp_get_status(tcp_conn_t *c)
{
	if (load_acquire(&c->pcb.state) < TCP_STATE_ESTABLISHED)
		return -EINPROGRESS;

	if (load_acquire(&c->tx_closed))
		return c->err ? -c->err : -EPIPE;

	return 0;
}

/**
 * tcp_conn_alloc - allocates a TCP connection struct
 *
 * Returns a connection, or NULL if out of memory.
 */
tcp_conn_t *tcp_conn_alloc(void)
{
	tcp_conn_t *c;

	c = kmalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return NULL;

	/* general fields */
	memset(&c->pcb, 0, sizeof(c->pcb));
	spin_lock_init(&c->lock);
	kref_init(&c->ref);
	c->err = 0;
	INIT_LIST_HEAD(&c->global_link);
	INIT_LIST_HEAD(&c->queue_link);

	/* ingress fields */
	c->rx_closed = false;
	c->rx_exclusive = false;
	tcp_wait_queue_init(&c->rx_wq);
	c->rxq_ooo_len = 0;
	INIT_LIST_HEAD(&c->rxq_ooo);
	INIT_LIST_HEAD(&c->rxq);

	/* egress fields */
	c->tx_closed = false;
	c->tx_exclusive = false;
	tcp_wait_queue_init(&c->tx_wq);
	c->tx_last_ack = 0;
	c->tx_last_win = 0;
	c->tx_pending = NULL;
	INIT_LIST_HEAD(&c->txq);
	c->do_fast_retransmit = false;
	INIT_WORK(&c->retransmit_work, tcp_retransmit);

	/* timeouts */
	c->next_timeout = -1L;
	c->ack_delayed = false;
	c->ack_ts = 0;
	c->time_wait_ts = 0;
	c->rep_acks = 0;
	c->acks_delayed_cnt = 0;
	c->zero_wnd = false;

	/* initialize egress PCB */
	c->pcb.state = TCP_STATE_CLOSED;
	c->pcb.iss = rand_crc32c(0x12345678); /* TODO: not enough */
	c->pcb.snd_nxt = c->pcb.iss;
	c->pcb.snd_una = c->pcb.iss;

	/* initialize ingress PCB */
	c->winmax = TCP_WIN;
	c->pcb.rcv_wscale = tcp_scale_window(TCP_WIN);
	c->pcb.rcv_wnd = TCP_WIN;
	c->pcb.rcv_mss = tcp_calculate_mss(FLUX_FNET_MTU);

	c->poll.set_fn = NULL;
	c->poll.clear_fn = NULL;

	return c;
}

/**
 * tcp_conn_attach - attaches a connection to the transport layer
 *
 * @c: the connection to attach
 * @laddr: the local network address
 * @raddr: the remote network address
 *
 * After calling this function, if successful, ingress packets and errors will
 * be delivered.
 */
int tcp_conn_attach(tcp_conn_t *c, struct net_addr laddr, struct net_addr raddr)
{
	int ret;
	struct task_struct *p = NULL;

	if (laddr.ip == 0)
		laddr.ip = fnet_dev->fast_net_ip;
	else if (laddr.ip != fnet_dev->fast_net_ip)
		return -EINVAL;

	trans_init_5tuple(&c->e, IPPROTO_TCP, &tcp_conn_ops, laddr, raddr);
	if (laddr.port == 0)
		ret = trans_table_add_with_ephemeral_port(&c->e);
	else
		ret = trans_table_add(&c->e);
	if (ret)
		return ret;

	spin_lock(&tcp_lock);
	swap(p, tcp_worker_blocked);
	list_add_tail(&c->global_link, &tcp_conns);
	spin_unlock(&tcp_lock);

	if (p) {
		pr_info("waking up tcp worker\n");
		wake_up_process(p);
	}

	c->attach_ts = now_us();

	return 0;
}

static void tcp_conn_release(struct rcu_head *h)
{
	tcp_conn_t *c = container_of(h, tcp_conn_t, e.rcu);

	spin_lock(&tcp_lock);
	list_del(&c->global_link);
	spin_unlock(&tcp_lock);

	if (c->tx_pending)
		mbuf_free(c->tx_pending);
	mbuf_list_free(&c->rxq_ooo);
	mbuf_list_free(&c->rxq);
	mbuf_list_free(&c->txq);
	kfree(c);
}

/**
 * tcp_conn_destroy - tears down a frees a TCP connection
 * @c: the connection to destroy
 */
void tcp_conn_destroy(tcp_conn_t *c)
{
	trans_table_remove(&c->e);
	call_rcu(&c->e.rcu, tcp_conn_release);
}

/**
 * tcp_conn_release_ref - a helper to free the conn when ref reaches zero
 * @r: the embedded reference count structure
 */
void tcp_conn_release_ref(struct kref *r)
{
	tcp_conn_t *c = container_of(r, tcp_conn_t, ref);

	BUG_ON(c->pcb.state != TCP_STATE_CLOSED);
	tcp_conn_destroy(c);
}

/*
 * Support for accepting new connections
 */

struct tcp_accq {
	struct trans_entry e;
	spinlock_t l;
	tcp_wait_queue_head_t wq;
	struct list_head conns;
	int backlog;
	bool shutdown;
	struct poll_head poll;
	struct kref ref;
};

struct net_addr flux_tcp_accq_local_addr(tcp_accq_t *q)
{
	return q->e.laddr;
}

int flux_tcp_accq_backlog(tcp_accq_t *q)
{
	return q->backlog;
}

static void tcp_accq_recv(struct trans_entry *e, struct mbuf *m)
{
	tcp_accq_t *q = container_of(e, tcp_accq_t, e);
	tcp_conn_t *c;
	tcp_wait_queue_entry_t *wq_entry;

	/* make sure the connection queue isn't full */
	spin_lock(&q->l);
	if (unlikely(q->backlog == 0 || q->shutdown)) {
		spin_unlock(&q->l);
		goto done;
	}
	q->backlog--;
	spin_unlock(&q->l);

	/* create a new connection */
	c = tcp_rx_listener(e->laddr, m);
	if (!c) {
		spin_lock(&q->l);
		q->backlog++;
		spin_unlock(&q->l);
		goto done;
	}

	/* wake a thread to accept the connection */
	spin_lock(&q->l);
	list_add_tail(&c->queue_link, &q->conns);
	wq_entry = tcp_wait_queue_signal_start(&q->wq);
	poll_set(&q->poll, POLLIN);
	spin_unlock(&q->l);
	tcp_wait_queue_signal_finish(wq_entry);

done:
	mbuf_free(m);
}

/* operations for TCP listen queues */
static const struct trans_ops tcp_accq_ops = {
	.recv = tcp_accq_recv,
};

static void tcp_accq_release(struct rcu_head *h)
{
	tcp_accq_t *q = container_of(h, tcp_accq_t, e.rcu);
	kfree(q);
}

static void tcp_accq_release_ref(struct kref *ref)
{
	tcp_accq_t *q = container_of(ref, tcp_accq_t, ref);
	call_rcu(&q->e.rcu, tcp_accq_release);
}

/**
 * flux_tcp_listen - creates a TCP listening queue for a local address
 *
 * @laddr: the local address to listen on
 * @backlog: the maximum number of unaccepted sockets to queue
 * @q_out: a pointer to store the newly created listening queue
 *
 * Returns 0 if successful, otherwise fails.
 */
int flux_tcp_listen(struct net_addr laddr, int backlog, tcp_accq_t **q_out)
{
	tcp_accq_t *q;
	int ret;

	if (backlog < 1)
		return -EINVAL;

	/* only can support one local IP so far */
	if (laddr.ip == 0)
		laddr.ip = fnet_dev->fast_net_ip;
	else if (laddr.ip != fnet_dev->fast_net_ip)
		return -EINVAL;

	pr_info("listening on %pI4:%u\n", &laddr.ip, laddr.port);

	q = kmalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	trans_init_3tuple(&q->e, IPPROTO_TCP, &tcp_accq_ops, laddr);
	spin_lock_init(&q->l);
	tcp_wait_queue_init(&q->wq);
	INIT_LIST_HEAD(&q->conns);
	q->backlog = backlog;
	q->shutdown = false;
	kref_init(&q->ref);

	q->poll.set_fn = NULL;
	q->poll.clear_fn = NULL;

	if (laddr.port == 0)
		ret = trans_table_add_with_ephemeral_port(&q->e);
	else
		ret = trans_table_add(&q->e);
	if (ret) {
		kfree(q);
		return ret;
	}

	*q_out = q;
	return 0;
}

/**
 * flux_tcp_accept - accepts a TCP connection
 * @q: the listen queue to accept the connection on
 * @nonblocking: true if this call should not block
 * @c_out: a pointer to store the connection
 *
 * Returns 0 if successful, otherwise -EPIPE if the listen queue was closed.
 */
int flux_tcp_accept(tcp_accq_t *q, bool nonblocking, tcp_conn_t **c_out)
{
	tcp_conn_t *c;

	spin_lock(&q->l);

	while (list_empty(&q->conns) && !q->shutdown) {
		if (nonblocking) {
			spin_unlock(&q->l);
			return -EAGAIN;
		}
		tcp_wait_queue_wait(&q->wq, &q->l, TASK_INTERRUPTIBLE);
	}

	/* was the queue drained and shutdown? */
	if (list_empty(&q->conns) && q->shutdown) {
		spin_unlock(&q->l);
		return -EPIPE;
	}

	/* otherwise a new connection is available */
	q->backlog++;
	c = list_first_entry_or_null(&q->conns, tcp_conn_t, queue_link);
	if (c)
		list_del(&c->queue_link);

	if (list_empty(&q->conns) && !q->shutdown)
		poll_clear(&q->poll, POLLIN);
	else if (!q->shutdown)
		poll_set(&q->poll, POLLIN);

	spin_unlock(&q->l);

	fnet_dbg("new conn 0x%llx accepted\n", (u64)c);

	*c_out = c;
	return 0;
}

static void tcp_accq_shutdown(tcp_accq_t *q)
{
	/* mark the listen queue as shutdown */
	spin_lock(&q->l);
	BUG_ON(q->shutdown);
	q->shutdown = true;
	poll_set(&q->poll, POLLRDHUP | POLLHUP | POLLIN);

	/* wake up all pending threads */
	tcp_wait_queue_wake_up_all(&q->wq);

	spin_unlock(&q->l);

	/* prevent ingress receive and error dispatch (after RCU period) */
	trans_table_remove(&q->e);
}

/**
 * flux_tcp_accq_shutdown - disables a TCP listener queue
 *
 * @q: the TCP listener queue to disable
 *
 * All blocking requests on the queue will return -EPIPE.
 */
void flux_tcp_accq_shutdown(tcp_accq_t *q)
{
	/* shutdown the listen queue */
	tcp_accq_shutdown(q);
}

/**
 * flux_tcp_accq_close - frees a TCP listener queue
 * @q: the TCP listener queue to close
 *
 * WARNING: Only the last reference can safely call this method. Call
 * flux_tcp_accq_shutdown() first if any threads are sleeping on the queue.
 */
void flux_tcp_accq_close(tcp_accq_t *q)
{
	tcp_conn_t *c, *tmp;

	if (!q->shutdown)
		tcp_accq_shutdown(q);

	/* free all pending connections */
	list_for_each_entry_safe(c, tmp, &q->conns, queue_link) {
		list_del(&c->queue_link);
		flux_tcp_close(c);
	}

	q->poll.set_fn = NULL;
	q->poll.clear_fn = NULL;

	kref_put(&q->ref, tcp_accq_release_ref);
}

/*
 * Support for the TCP socket API
 */

static int __tcp_dial(struct net_addr laddr, struct net_addr raddr,
		      tcp_conn_t **c_out, bool nonblocking)
{
	struct tcp_options opts;
	tcp_conn_t *c;
	int ret;

	if (raddr.ip == INADDR_LOOPBACK)
		return -EADDRNOTAVAIL;

	/* create and initialize a connection */
	c = tcp_conn_alloc();
	if (unlikely(!c))
		return -ENOMEM;

	/*
	 * Attach the connection to the transport layer. From this point onward
	 * ingress packets can be dispatched to the connection.
	 */
	ret = tcp_conn_attach(c, laddr, raddr);
	if (unlikely(ret)) {
		kfree(c);
		return ret;
	}

	opts.opt_en = (TCPOPT_MSS_ENBIT | TCPOPT_WINDOW_ENBIT);
	opts.mss = c->pcb.rcv_mss;
	opts.wscale = c->pcb.rcv_wscale;

	/* send a SYN to the remote host */
	spin_lock(&c->lock);
	ret = tcp_tx_ctl(c, TCP_SYN, &opts);
	if (unlikely(ret)) {
		tcp_conn_fail(c, ret);
		tcp_conn_put(c); /* release user socket ref */
		spin_unlock(&c->lock);
		return ret;
	}
	tcp_conn_get(c); /* take a ref for the state machine */
	tcp_conn_set_state(c, TCP_STATE_SYN_SENT);

	if (nonblocking) {
		spin_unlock(&c->lock);
		*c_out = c;
		return -EINPROGRESS;
	}

	/* wait until the connection is established or there is a failure */
	while (!c->tx_closed && c->pcb.state < TCP_STATE_ESTABLISHED) {
		tcp_wait_queue_wait(&c->tx_wq, &c->lock, TASK_INTERRUPTIBLE);
	}

	/* check if the connection failed */
	if (c->tx_closed) {
		ret = -c->err;
		tcp_conn_fail(c, c->err);
		tcp_conn_put(c); /* release user socket ref */
		spin_unlock(&c->lock);
		return ret;
	}
	spin_unlock(&c->lock);

	*c_out = c;
	return 0;
}

/**
 * tcp_dial - opens a TCP connection, creating a new socket
 * @laddr: the local address
 * @raddr: the remote address
 * @c_out: a pointer to store the new connection
 *
 * Returns 0 if successful, otherwise fail.
 */
int flux_tcp_dial(struct net_addr laddr, struct net_addr raddr,
		 tcp_conn_t **c_out)
{
	return __tcp_dial(laddr, raddr, c_out, false);
}

/**
 * tcp_dial_nonblocking - opens a nonblocking TCP connection, creating a new
 * socket
 * @laddr: the local address
 * @raddr: the remote address
 * @c_out: a pointer to store the new connection
 *
 * Returns 0 if successful, otherwise fail.
 */
int flux_tcp_dial_nonblocking(struct net_addr laddr, struct net_addr raddr,
			     tcp_conn_t **c_out)
{
	return __tcp_dial(laddr, raddr, c_out, true);
}

/**
 * tcp_local_addr - gets the local address of a TCP connection
 * @c: the TCP connection
 */
struct net_addr flux_tcp_local_addr(tcp_conn_t *c)
{
	return c->e.laddr;
}

/**
 * tcp_remote_addr - gets the remote address of a TCP connection
 * @c: the TCP connection
 */
struct net_addr flux_tcp_remote_addr(tcp_conn_t *c)
{
	return c->e.raddr;
}

/**
 * tcp_wait_rx - blocks until there is an actionable event.
 * @c: the TCP connection
 *
 * Returns 1 if there is data ready. Returns 0 or a negative number
 * if there is an error.
 */
static int tcp_wait_rx(tcp_conn_t *c, bool nonblocking)
{
	/* block until there is an actionable event */
	while (!c->rx_closed && (c->rx_exclusive || list_empty(&c->rxq))) {
		if (nonblocking) {
			spin_unlock(&c->lock);
			return -EAGAIN;
		}
		tcp_wait_queue_wait(&c->rx_wq, &c->lock, TASK_INTERRUPTIBLE);
	}

	if (c->rx_closed) {
		spin_unlock(&c->lock);
		return -c->err;
	}

	return 1;
}

/**
 * tcp_read_peek - reads data from a TCP connection without consuming the data.
 * @c: the TCP connection
 * @buf: a buffer to store the read data
 * @len: the length of @buf
 * @nonblocking: true if this call should not block
 *
 * Returns the number of bytes read, 0 if the connection is closed, or < 0
 * if an error occurred.
 */
static ssize_t tcp_read_peek(tcp_conn_t *c, void *buf, size_t len,
			     bool nonblocking)
{
	int ret;
	struct mbuf *m;
	size_t tocopy, readlen = 0;

	spin_lock(&c->lock);

	ret = tcp_wait_rx(c, nonblocking);
	if (unlikely(ret <= 0))
		return ret;

	list_for_each_entry(m, &c->rxq, link) {
		tocopy = min((size_t)mbuf_length(m), len - readlen);
		memcpy(buf + readlen, mbuf_data(m), tocopy);
		readlen += tocopy;

		if (len == readlen)
			break;
	}

	spin_unlock(&c->lock);
	return readlen;
}

static size_t iov_len(const struct iovec *iov, int iovcnt)
{
	size_t len = 0;
	int i;

	for (i = 0; i < iovcnt; i++)
		len += iov[i].iov_len;

	return len;
}

/**
 * tcp_readv_peek - reads vectorized data from a TCP connection without
 * consuming the data.
 * @c: the TCP connection
 * @iov: a pointer to the IO vector
 * @iovcnt: the number of vectors in @iov
 * @nonblocking: true if this call should not block
 *
 * Returns the number of bytes read, 0 if the connection is closed, or < 0
 * if an error occurred.
 */
static ssize_t tcp_readv_peek(tcp_conn_t *c, const struct iovec *iov,
			      int iovcnt, bool nonblocking)
{
	int ret, i = 0;
	struct mbuf *m;
	const struct iovec *vp;
	size_t tocopy, readlen = 0;
	off_t mbuf_off = 0, iov_off = 0;

	spin_lock(&c->lock);

	ret = tcp_wait_rx(c, nonblocking);
	if (unlikely(ret <= 0))
		return ret;

	m = list_first_entry(&c->rxq, struct mbuf, link);
	vp = &iov[0];
	while (1) {
		tocopy = min(vp->iov_len - iov_off,
			     (size_t)mbuf_length(m) - mbuf_off);
		memcpy((char *)vp->iov_base + iov_off, mbuf_data(m) + mbuf_off,
		       tocopy);

		iov_off += tocopy;
		mbuf_off += tocopy;
		readlen += tocopy;

		if (mbuf_off == mbuf_length(m)) {
			if (list_is_last(&m->link, &c->rxq))
				break;
			m = list_next_entry(m, link);
			mbuf_off = 0;
		}

		if (iov_off == vp->iov_len) {
			if (++i == iovcnt)
				break;
			iov_off = 0;
			vp = &iov[i];
		}
	}

	spin_unlock(&c->lock);
	return readlen;
}

static ssize_t tcp_read_wait(tcp_conn_t *c, size_t len, struct list_head *q,
			     struct mbuf **mout, bool nonblocking)
{
	int ret;
	struct mbuf *m;
	size_t readlen = 0;
	bool do_ack = false;

	*mout = NULL;
	spin_lock(&c->lock);

	ret = tcp_wait_rx(c, nonblocking);
	if (unlikely(ret <= 0))
		return ret;

	/* pop off the mbufs that will be read */
	while (readlen < len) {
		m = list_first_entry_or_null(&c->rxq, struct mbuf, link);
		if (!m)
			break;

		if (unlikely((m->flags & TCP_FIN) > 0)) {
			tcp_conn_shutdown_rx(c);
			if (mbuf_length(m) == 0)
				break;
		}

		if (len - readlen < mbuf_length(m)) {
			c->rx_exclusive = true;
			*mout = m;
			readlen = len;
			break;
		}

		list_del(&m->link);
		list_add_tail(&m->link, q);
		readlen += mbuf_length(m);
	}

	c->pcb.rcv_wnd += readlen;
	if (wraps_gte(c->pcb.rcv_nxt + c->pcb.rcv_wnd,
		      c->tx_last_ack + c->tx_last_win + c->winmax / 4)) {
		do_ack = true;
	}

	if (list_empty(&c->rxq))
		poll_clear(&c->poll, POLLIN);

	spin_unlock(&c->lock);

	if (do_ack)
		tcp_tx_ack(c);
	return readlen;
}

static void tcp_read_finish(tcp_conn_t *c, struct mbuf *m)
{
	struct list_head waiters;

	if (!m)
		return;

	INIT_LIST_HEAD(&waiters);
	spin_lock(&c->lock);
	c->rx_exclusive = false;
	tcp_wait_queue_wake_up_all_start(&c->rx_wq, &waiters);
	spin_unlock(&c->lock);
	tcp_wait_queue_wake_up_all_finish(&waiters);
}

/**
 * flux_tcp_read2 - reads data from a TCP connection
 * @c: the TCP connection
 * @buf: a buffer to store the read data
 * @len: the length of @buf
 * @nonblocking: true if this call should not block
 *
 * Returns the number of bytes read, 0 if the connection is closed, or < 0
 * if an error occurred.
 */
ssize_t flux_tcp_read2(tcp_conn_t *c, void *buf, size_t len, bool peek,
		      bool nonblocking)
{
	char *pos = buf;
	struct list_head q;
	struct mbuf *m, *cur;
	ssize_t ret;

	if (peek)
		return tcp_read_peek(c, buf, len, nonblocking);

	INIT_LIST_HEAD(&q);

	/* wait for data to become available */
	ret = tcp_read_wait(c, len, &q, &m, nonblocking);

	/* check if connection was closed */
	if (ret <= 0)
		return ret;

	/* copy the data from the buffers */
	while (1) {
		cur = list_first_entry_or_null(&q, struct mbuf, link);
		if (!cur)
			break;
		list_del(&cur->link);

		memcpy(pos, mbuf_data(cur), mbuf_length(cur));
		pos += mbuf_length(cur);
		mbuf_free(cur);
	}

	/* we may have to consume only part of a buffer */
	if (m) {
		size_t cpylen = len - (uintptr_t)pos + (uintptr_t)buf;
		memcpy(pos, mbuf_pull(m, cpylen), cpylen);
		m->seg_seq += cpylen;
	}

	/* wakeup any pending readers */
	tcp_read_finish(c, m);

	return ret;
}

/**
 * tcp_readv - reads vectored data from a TCP connection
 * @c: the TCP connection
 * @iov: a pointer to the IO vector
 * @iovcnt: the number of vectors in @iov
 * @nonblocking: true if this call should not block
 *
 * Returns the number of bytes read, 0 if the connection is closed, or < 0
 * if an error occurred.
 */
ssize_t flux_tcp_readv2(tcp_conn_t *c, const struct iovec *iov, int iovcnt,
		       bool peek, bool nonblocking)
{
	struct list_head q;
	struct mbuf *m, *cur;
	ssize_t len;
	off_t offset;
	int i;
	const struct iovec *vp;
	size_t cpylen;

	if (peek)
		return tcp_readv_peek(c, iov, iovcnt, nonblocking);

	INIT_LIST_HEAD(&q);
	offset = 0;
	i = 0;
	len = iov_len(iov, iovcnt);

	/* wait for data to become available */
	len = tcp_read_wait(c, len, &q, &m, nonblocking);

	/* check if connection was closed */
	if (len <= 0)
		return len;

	/* copy the data from the buffers */
	while (1) {
		cur = list_first_entry_or_null(&q, struct mbuf, link);
		if (!cur)
			break;
		list_del(&cur->link);

		do {
			vp = &iov[i];
			cpylen = min(vp->iov_len - offset,
				     (size_t)mbuf_length(cur));

			memcpy((char *)vp->iov_base + offset,
			       mbuf_pull(cur, cpylen), cpylen);

			offset += cpylen;
			if (offset == vp->iov_len) {
				offset = 0;
				i++;
			}
		} while (mbuf_length(cur) > 0);
		mbuf_free(cur);
	}

	/* we may have to consume only part of a buffer */
	if (m) {
		do {
			vp = &iov[i];
			cpylen = min(vp->iov_len - offset,
				     (size_t)mbuf_length(m));

			memcpy((char *)vp->iov_base + offset,
			       mbuf_pull(m, cpylen), cpylen);
			m->seg_seq += cpylen;
			offset += cpylen;
			if (offset == vp->iov_len) {
				offset = 0;
				i++;
			}
		} while (i < iovcnt);
	}

	/* wakeup any pending readers */
	tcp_read_finish(c, m);

	return len;
}

static int tcp_write_wait(tcp_conn_t *c, size_t *winlen, bool nonblocking)
{
	spin_lock(&c->lock);

	/* block until there is an actionable event */
	while (!c->tx_closed && (c->pcb.state < TCP_STATE_ESTABLISHED ||
				 c->tx_exclusive || tcp_is_snd_full(c))) {
		/* arm window probing if needed */
		if (!c->zero_wnd && tcp_is_snd_full(c)) {
			c->zero_wnd = true;
			c->zero_wnd_ts = now_us();
			tcp_timer_update(c);
		}

		if (nonblocking) {
			spin_unlock(&c->lock);
			return -EAGAIN;
		}

		tcp_wait_queue_wait(&c->tx_wq, &c->lock, TASK_INTERRUPTIBLE);
	}
	c->zero_wnd = false;

	/* is the socket closed? */
	if (c->tx_closed) {
		spin_unlock(&c->lock);
		return c->err ? -c->err : -EPIPE;
	}

	/* drop the lock to allow concurrent RX processing */
	c->tx_exclusive = true;

	*winlen = c->pcb.snd_una + c->pcb.snd_wnd - c->pcb.snd_nxt;
	c->acks_delayed_cnt = 0;
	c->ack_delayed = false;
	spin_unlock(&c->lock);

	return 0;
}

static void tcp_write_finish(tcp_conn_t *c)
{
	struct list_head q;
	struct list_head waiters;
	struct mbuf *retransmit = NULL;

	INIT_LIST_HEAD(&q);
	INIT_LIST_HEAD(&waiters);

	spin_lock(&c->lock);
	c->tx_exclusive = false;
	tcp_conn_ack(c, &q);
	if (c->pcb.rcv_nxt == c->tx_last_ack) /* race condition check */
		c->ack_delayed = false;
	else
		c->ack_ts = now_us();
	if (c->pcb.state == TCP_STATE_CLOSED) {
		list_splice_init(&c->txq, &q);
		if (c->tx_pending) {
			list_add_tail(&c->tx_pending->link, &q);
			c->tx_pending = NULL;
		}
	} else if (c->do_fast_retransmit) {
		c->do_fast_retransmit = false;
		if (c->fast_retransmit_last_ack == c->pcb.snd_una)
			retransmit = tcp_tx_fast_retransmit_start(c);
	}

	tcp_timer_update(c);
	tcp_wait_queue_wake_up_all_start(&c->tx_wq, &waiters);

	if (tcp_is_snd_full(c))
		poll_clear(&c->poll, POLLOUT);

	spin_unlock(&c->lock);

	tcp_tx_fast_retransmit_finish(c, retransmit);
	tcp_wait_queue_wake_up_all_finish(&waiters);
	mbuf_list_free(&q);
}

/**
 * flux_tcp_write2 - writes data to a TCP connection
 * @c: the TCP connection
 * @buf: a buffer from which to copy the data
 * @len: the length of the data
 * @nonblocking: true if this call should not block
 *
 * Returns the number of bytes written (could be less than @len), or < 0
 * if there was a failure.
 */
ssize_t flux_tcp_write2(tcp_conn_t *c, const void *buf, size_t len,
		       bool nonblocking)
{
	size_t winlen;
	ssize_t ret;

	/* block until the data can be sent */
	ret = tcp_write_wait(c, &winlen, nonblocking);
	if (ret)
		return ret;

	/* actually send the data */
	ret = tcp_tx_send(c, buf, min(len, winlen), true);

	/* catch up on any pending work */
	tcp_write_finish(c);

	return ret;
}

/**
 * tcp_writev2 - writes vectored data to a TCP connection
 * @c: the TCP connection
 * @iov: a pointer to the IO vector
 * @iovcnt: the number of vectors in @iov
 * @nonblocking: true if this call should not block
 *
 * Returns the number of bytes written (could be less than requested), or < 0
 * if there was a failure.
 */
ssize_t flux_tcp_writev2(tcp_conn_t *c, const struct iovec *iov, int iovcnt,
			bool nonblocking)
{
	size_t winlen;
	ssize_t sent = 0, ret;
	int i;

	/* block until the data can be sent */
	ret = tcp_write_wait(c, &winlen, nonblocking);
	if (ret)
		return ret;

	/* actually send the data */
	for (i = 0; i < iovcnt; i++, iov++) {
		if (winlen <= 0)
			break;
		ret = tcp_tx_send(c, iov->iov_base, min(iov->iov_len, winlen),
				  i == iovcnt - 1 && iov->iov_len <= winlen);
		if (ret <= 0)
			break;
		winlen -= ret;
		sent += ret;
	}

	/* catch up on any pending work */
	tcp_write_finish(c);

	return sent > 0 ? sent : ret;
}

/* resend any pending egress packets that timed out */
static void tcp_retransmit(struct work_struct *w)
{
	tcp_conn_t *c = container_of(w, tcp_conn_t, retransmit_work);

	spin_lock(&c->lock);

	while (c->tx_exclusive && c->pcb.state != TCP_STATE_CLOSED)
		tcp_wait_queue_wait(&c->tx_wq, &c->lock, TASK_UNINTERRUPTIBLE);

	if (c->pcb.state != TCP_STATE_CLOSED) {
		c->tx_exclusive = true;
		spin_unlock(&c->lock);
		tcp_tx_retransmit(c);
		tcp_write_finish(c);
	} else {
		spin_unlock(&c->lock);
	}

	tcp_conn_put(c);
}

/**
 * tcp_conn_fail - closes a TCP both sides of a connection with an error
 * @c: the TCP connection to shutdown
 * @err: the error code (failure reason for the close)
 *
 * The caller must hold @c's lock.
 */
void tcp_conn_fail(tcp_conn_t *c, int err)
{
	bool was_closed = c->pcb.state == TCP_STATE_CLOSED;

	c->err = err;
	tcp_conn_set_state(c, TCP_STATE_CLOSED);

	tcp_conn_shutdown_rx(c);

	if (!c->tx_closed) {
		store_release(&c->tx_closed, true);
		tcp_wait_queue_wake_up_all(&c->tx_wq);
		poll_set(&c->poll, POLLHUP);
	}

	/* will be freed by the writer if one is busy */
	if (!c->tx_exclusive) {
		mbuf_list_free(&c->txq);
		if (c->tx_pending) {
			mbuf_free(c->tx_pending);
			c->tx_pending = NULL;
		}
	}
	if (!c->rx_exclusive)
		mbuf_list_free(&c->rxq);
	mbuf_list_free(&c->rxq_ooo);

	/* state machine is disabled, drop ref */
	if (!was_closed)
		tcp_conn_put(c);
}

/**
 * tcp_conn_shutdown_rx - closes ingress for a TCP connection
 * @c: the TCP connection to shutdown
 *
 * The caller must hold @c's lock.
 */
void tcp_conn_shutdown_rx(tcp_conn_t *c)
{
	if (c->rx_closed)
		return;

	poll_set(&c->poll, POLLRDHUP | POLLIN);
	c->rx_closed = true;
	tcp_wait_queue_wake_up_all(&c->rx_wq);
}

static int tcp_conn_shutdown_tx(tcp_conn_t *c, bool interruptible)
{
	int ret;

	if (c->tx_closed)
		return 0;

	if (unlikely(c->pcb.state < TCP_STATE_ESTABLISHED)) {
		tcp_conn_fail(c, ECONNABORTED);
		tcp_tx_raw_rst(c->e.laddr, c->e.raddr, c->pcb.snd_nxt);
		return 0;
	}

	while (c->tx_exclusive) {
		tcp_wait_queue_wait(&c->tx_wq, &c->lock,
				    interruptible ? TASK_INTERRUPTIBLE :
						    TASK_UNINTERRUPTIBLE);
	}

	ret = tcp_tx_ctl(c, TCP_FIN | TCP_ACK, NULL);
	if (unlikely(ret)) {
		tcp_conn_fail(c, ret);
		return 0;
	}
	if (c->pcb.state == TCP_STATE_ESTABLISHED)
		tcp_conn_set_state(c, TCP_STATE_FIN_WAIT1);
	else if (c->pcb.state == TCP_STATE_CLOSE_WAIT)
		tcp_conn_set_state(c, TCP_STATE_LAST_ACK);
	else
		BUG();

	poll_set(&c->poll, POLLHUP);
	c->tx_closed = true;
	tcp_wait_queue_wake_up_all(&c->tx_wq);

	return 0;
}

/**
 * flux_tcp_shutdown - shuts a TCP connection down
 * @c: the TCP connection to shutdown
 * @how: the directions to shutdown (SHUT_RD, SHUT_WR, or SHUT_RDWR)
 *
 * Returns 0 if successful, otherwise < 0 for failure.
 */
int flux_tcp_shutdown(tcp_conn_t *c, int how)
{
	bool tx, rx;
	int ret;

	if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
		return -EINVAL;

	tx = how == SHUT_WR || how == SHUT_RDWR;
	rx = how == SHUT_RD || how == SHUT_RDWR;

	spin_lock(&c->lock);
	if (tx) {
		ret = tcp_conn_shutdown_tx(c, true);
		if (ret) {
			spin_unlock(&c->lock);
			return ret;
		}
	}
	if (rx)
		tcp_conn_shutdown_rx(c);
	spin_unlock(&c->lock);

	return 0;
}

/**
 * flux_tcp_abort - force an immediate (ungraceful) close of the connection
 *
 * @c: the TCP connection to abort
 */
void flux_tcp_abort(tcp_conn_t *c)
{
	uint32_t snd_nxt;
	struct net_addr l, r;

	spin_lock(&c->lock);
	if (c->pcb.state == TCP_STATE_CLOSED) {
		spin_unlock(&c->lock);
		return;
	}

	l = c->e.laddr;
	r = c->e.raddr;
	tcp_conn_fail(c, ECONNABORTED);

	while (c->tx_exclusive)
		tcp_wait_queue_wait(&c->tx_wq, &c->lock, TASK_UNINTERRUPTIBLE);

	snd_nxt = c->pcb.snd_nxt;
	spin_unlock(&c->lock);
	tcp_tx_raw_rst(l, r, snd_nxt);
}

/**
 * flux_tcp_close - frees a TCP connection
 *
 * @c: the TCP connection to free
 *
 * WARNING: Only the last reference can safely call this method. Call
 * flux_tcp_shutdown() or flux_tcp_abort() first if any threads are sleeping on the
 * socket.
 */
void flux_tcp_close(tcp_conn_t *c)
{
	int ret;

	spin_lock(&c->lock);
	ret = tcp_conn_shutdown_tx(c, false);
	if (ret)
		tcp_conn_fail(c, -ret);
	tcp_conn_shutdown_rx(c);

	c->poll.set_fn = NULL;
	c->poll.clear_fn = NULL;

	spin_unlock(&c->lock);

	tcp_conn_put(c);
}

void flux_tcp_poll_install_cb(tcp_conn_t *c, poll_notif_fn_t setfn,
			     poll_notif_fn_t clearfn, unsigned long data)
{
	unsigned int flags = 0;

	spin_lock(&c->lock);
	c->poll.set_fn = setfn;
	c->poll.clear_fn = clearfn;
	c->poll.data = data;

	if (c->pcb.state == TCP_STATE_ESTABLISHED && !tcp_is_snd_full(c))
		flags |= POLLOUT;

	if (!list_empty(&c->rxq))
		flags |= POLLIN;

	if (c->rx_closed)
		flags |= POLLRDHUP;

	if (c->tx_closed)
		flags |= POLLHUP;

	poll_set(&c->poll, flags);

	spin_unlock(&c->lock);
}

void flux_tcp_accq_poll_install_cb(tcp_accq_t *q, poll_notif_fn_t setfn,
				  poll_notif_fn_t clearfn, unsigned long data)
{
	unsigned int flags = 0;

	spin_lock(&q->l);
	q->poll.set_fn = setfn;
	q->poll.clear_fn = clearfn;
	q->poll.data = data;

	if (!list_empty(&q->conns))
		flags |= POLLIN;

	if (q->shutdown)
		flags |= POLLIN | POLLRDHUP | POLLHUP;

	poll_set(&q->poll, flags);

	spin_unlock(&q->l);
}

/**
 * flux_tcp_init - starts the TCP worker thread
 *
 * Returns 0 if successful.
 */
int flux_tcp_init(void)
{
	kthread_run(tcp_worker, NULL, "tcp_worker");
	return 0;
}
