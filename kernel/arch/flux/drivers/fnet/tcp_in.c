/*
 * tcp_in.c - the ingress datapath for TCP
 *
 * Based on RFC 793 and RFC 1122 (errata).
 *
 * FIXME: We do too little to prevent heavy fragmentation in the out-of-order
 * RX queue.
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <linux/list.h>
#include <linux/wait.h>
#include <net/tcp.h>
#include <net/tcp_states.h>
#include <uapi/linux/tcp.h>

#include "fnet.h"
#include "core.h"
#include "tcp.h"

#define TCP_SLOWPATH_FLAGS (TCP_FIN | TCP_RST | TCP_URG)

static void __tcp_rx_conn(tcp_conn_t *c, struct mbuf *m, uint32_t ack,
			  uint32_t snd_nxt, uint32_t win,
			  const unsigned char *optp, int optlen);

/* four cases for the acceptability test for an incoming segment */
static bool is_acceptable(tcp_conn_t *c, uint32_t len, uint32_t seq,
			  uint8_t flags)
{
	if (len == 0 && c->pcb.rcv_wnd == 0) {
		/* RFC 793 section 3.9 page 69: If the RCV.WND is zero,
                  no segments will be acceptable, but special allowance
                  should be made to accept valid ACKs, URGs and RSTs. */
		if ((flags & TCP_ACK) || (flags & TCP_URG) || (flags & TCP_RST))
			return true;
		return seq == c->pcb.rcv_nxt;
	} else if (len == 0 && c->pcb.rcv_wnd > 0) {
		return wraps_lte(c->pcb.rcv_nxt, seq) &&
		       wraps_lt(seq, c->pcb.rcv_nxt + c->pcb.rcv_wnd);
	} else if (len > 0 && c->pcb.rcv_wnd == 0) {
		return false;
	}

	/* (len > 0 && c->rcv_wnd > 0) */
	return (wraps_lte(c->pcb.rcv_nxt, seq) &&
		wraps_lt(seq, c->pcb.rcv_nxt + c->pcb.rcv_wnd)) ||
	       (wraps_lte(c->pcb.rcv_nxt, seq + len - 1) &&
		wraps_lt(seq + len - 1, c->pcb.rcv_nxt + c->pcb.rcv_wnd));
}

/* see reset generation (RFC 793) */
static void send_rst(tcp_conn_t *c, bool acked, uint32_t seq, uint32_t ack,
		     uint32_t len)
{
	if (acked) {
		tcp_tx_raw_rst(c->e.laddr, c->e.raddr, ack);
		return;
	}
	tcp_tx_raw_rst_ack(c->e.laddr, c->e.raddr, 0, seq + len);
}

static void tcp_rx_append_text(tcp_conn_t *c, struct mbuf *m)
{
	uint64_t nxt_wnd;
	uint32_t len;

	/* does the next receive octet clip the head of the text? */
	if (wraps_lt(m->seg_seq, c->pcb.rcv_nxt)) {
		len = c->pcb.rcv_nxt - m->seg_seq;
		mbuf_pull(m, len);
		m->seg_seq += len;
	}

	/* does the receive window clip the tail of the text? */
	if (wraps_lt(c->pcb.rcv_nxt + c->pcb.rcv_wnd, m->seg_end)) {
		len = m->seg_end - (c->pcb.rcv_nxt + c->pcb.rcv_wnd);
		mbuf_trim(m, len);
		m->seg_end = c->pcb.rcv_nxt + c->pcb.rcv_wnd;
	}

	/* enqueue the text */
	nxt_wnd = (uint64_t)m->seg_end;
	nxt_wnd |=
		((uint64_t)(c->pcb.rcv_wnd - (m->seg_end - m->seg_seq)) << 32);
	store_release(&c->pcb.rcv_nxt_wnd, nxt_wnd);
	list_add_tail(&m->link, &c->rxq);
}

/* process RX text segments, returning true if @m is used for text */
static bool tcp_rx_text(tcp_conn_t *c, struct mbuf *m, bool *wake, bool *fin)
{
	struct mbuf *pos;

	/* don't accept any text if the receive window is zero */
	if (c->pcb.rcv_wnd == 0)
		return false;

	if (wraps_lte(m->seg_seq, c->pcb.rcv_nxt)) {
		/* we got the next in-order segment */
		if ((m->flags & (TCP_PUSH | TCP_FIN)) != 0 ||
		    !list_empty(&c->rxq)) {
			*wake = true;
			*fin |= (m->flags & TCP_FIN) > 0;
		}
		tcp_rx_append_text(c, m);
	} else {
		/* we got an out-of-order segment */
		if (c->rxq_ooo_len >= TCP_OOO_MAX_SIZE)
			return false;

		list_for_each_entry_reverse(pos, &c->rxq_ooo, link) {
			if (wraps_gt(m->seg_end, pos->seg_end)) {
				list_add(&m->link, &pos->link);
				c->rxq_ooo_len++;
				goto drain;
			} else if (wraps_gte(m->seg_seq, pos->seg_seq)) {
				return false;
			}
		}

		list_add(&m->link, &c->rxq_ooo);
		c->rxq_ooo_len++;
	}

drain:
	/* attempt to drain the out-of-order RX queue */
	while (true) {
		pos = list_first_entry_or_null(&c->rxq_ooo, struct mbuf, link);
		if (!pos)
			break;

		/* has the segment been fully received already? */
		if (wraps_lte(pos->seg_end, c->pcb.rcv_nxt)) {
			list_del(&pos->link);
			c->rxq_ooo_len--;
			mbuf_free(pos);
			continue;
		}

		/* is the segment still out-of-order? */
		if (wraps_gt(pos->seg_seq, c->pcb.rcv_nxt))
			break;

		/* we got the next in-order segment */
		list_del(&pos->link);
		c->rxq_ooo_len--;
		*wake = true;
		*fin |= (pos->flags & TCP_FIN) > 0;
		tcp_rx_append_text(c, pos);
	}

	if (c->pcb.rcv_wnd == 0)
		*wake = true;

	return true;
}

/* fast path for handling ingress packets for TCP connections */
void tcp_rx_conn(struct trans_entry *e, struct mbuf *m)
{
	tcp_conn_t *c = container_of(e, tcp_conn_t, e);
	struct list_head q = LIST_HEAD_INIT(q);
	const struct iphdr *iphdr;
	const struct tcphdr *tcphdr;
	const unsigned char *optp;
	int optlen;
	uint64_t nxt_wnd;
	uint32_t seq, ack, len, snd_nxt, hdr_len, win;
	bool do_ack = false, slow_path;
	tcp_wait_queue_entry_t *wq_entry = NULL;

	snd_nxt = load_acquire(&c->pcb.snd_nxt);

	/* find header offsets */
	iphdr = mbuf_network_hdr(m, *iphdr);
	mbuf_mark_transport_offset(m);
	tcphdr = mbuf_pull_hdr_or_null(m, *tcphdr);
	if (unlikely(!tcphdr)) {
		mbuf_free(m);
		return;
	}

	/* parse header */
	seq = ntohl(tcphdr->seq);
	ack = ntohl(tcphdr->ack_seq);
	win = (uint32_t)ntohs(tcphdr->window) << c->pcb.snd_wscale;
	hdr_len = tcphdr->doff * sizeof(uint32_t);
	if (unlikely(hdr_len < sizeof(struct tcphdr))) {
		mbuf_free(m);
		return;
	}
	len = ntohs(iphdr->tot_len) - sizeof(*iphdr) - hdr_len;
	if (unlikely(len > mbuf_length(m) || len > c->pcb.rcv_mss)) {
		mbuf_free(m);
		return;
	}

	m->seg_seq = seq;
	m->seg_end = seq + len;
	m->flags = tcp_flag_byte(tcphdr);

	/* pull off options */
	optlen = hdr_len - sizeof(struct tcphdr);
	optp = mbuf_pull(m, optlen);

	/* Use slow path if not regular flags and the next segment */
	slow_path = (tcp_flag_byte(tcphdr) & TCP_SLOWPATH_FLAGS) != 0 ||
		    (len == 0) || wraps_gt(ack, snd_nxt);

	spin_lock(&c->lock);

	/* Is the connection in the established state? */
	slow_path |= (c->pcb.state != TCP_STATE_ESTABLISHED);

	/* Might we need to unblock waiting senders? */
	slow_path |= tcp_is_snd_full(c);

	/* Is the packet on the next in-order boundary? */
	slow_path |= (seq != c->pcb.rcv_nxt) || !list_empty(&c->rxq_ooo);

	/* Does it fit perfectly in the receive window? */
	slow_path |= c->pcb.rcv_wnd < len;

	/* Does the ack land outside snd_nxt? */
	slow_path |= wraps_gt(ack, snd_nxt);

	if (unlikely(slow_path)) {
		fnet_dbg("conn rx slow path for %llx %x\n", (u64)m, m->flags);
		return __tcp_rx_conn(c, m, ack, snd_nxt, win, optp, optlen);
	}

	/* process acks and update send window */
	if (wraps_lte(c->pcb.snd_una, ack)) {
		/* did sent segments get acked? */
		if (c->pcb.snd_una != ack) {
			c->rep_acks = 0;
			c->pcb.snd_una = ack;
			tcp_conn_ack(c, &q);
		}

		/* should we update the send window? */
		if (wraps_lt(c->pcb.snd_wl1, seq) ||
		    (c->pcb.snd_wl1 == seq && wraps_lte(c->pcb.snd_wl2, ack))) {
			c->pcb.snd_wnd = win;
			c->pcb.snd_wl1 = seq;
			c->pcb.snd_wl2 = ack;
			c->rep_acks = 0;
		}
	}

	nxt_wnd = (uint64_t)m->seg_end;
	nxt_wnd |= ((uint64_t)(c->pcb.rcv_wnd - len) << 32);
	store_release(&c->pcb.rcv_nxt_wnd, nxt_wnd);

	/* should we wake a thread */
	if (!list_empty(&c->rxq) || (tcp_flag_byte(tcphdr) & TCP_PUSH) > 0) {
		wq_entry = tcp_wait_queue_signal_start(&c->rx_wq);
		poll_set(&c->poll, POLLIN);
	}

	/* handle delayed acks */
	if (++c->acks_delayed_cnt >= 2) {
		c->ack_delayed = false;
		do_ack = true;
		c->acks_delayed_cnt = 0;
	} else if (!c->ack_delayed) {
		c->ack_ts = ktime_to_us(ktime_get());
		c->ack_delayed = true;
		c->next_timeout =
			min(c->next_timeout, c->ack_ts + TCP_ACK_TIMEOUT);
	}

	list_add_tail(&m->link, &c->rxq);
	spin_unlock(&c->lock);

	/* deferred work (delayed until after the lock was dropped) */
	if (wq_entry)
		tcp_wait_queue_signal_finish(wq_entry);
	mbuf_list_free(&q);
	if (do_ack)
		tcp_tx_ack(c);
}

static int __tcp_parse_options(tcp_conn_t *c, const unsigned char *ptr, int len)
{
	int opt_en = 0;
	uint16_t mss = 0;
	uint8_t wscale = 0;

	while (len > 0) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			goto done;
		case TCPOPT_NOP:
			len--;
			continue;
		case TCPOPT_MSS:
			opsize = *ptr++;
			if (opsize == TCPOLEN_MSS) {
				mss = ntohs(*(uint16_t *)ptr);
				opt_en |= TCPOPT_MSS_ENBIT;
			}
			break;
		case TCPOPT_WINDOW:
			opsize = *ptr++;
			if (opsize == TCPOLEN_WINDOW) {
				wscale = *(uint8_t *)ptr;
				if (wscale > 14)
					wscale = 14;
				opt_en |= TCPOPT_WINDOW_ENBIT;
			}
			break;
		default:
			opsize = *ptr++;
		}
		ptr += opsize - 2;
		len -= opsize;
	}

done:
	c->pcb.snd_mss =
		min((uint32_t)max(mss, (uint16_t)TCP_MIN_MSS), c->pcb.rcv_mss);
	c->pcb.snd_wscale = wscale;
	if (!(opt_en & TCPOPT_WINDOW_ENBIT)) {
		c->pcb.rcv_wnd = c->winmax = min(c->winmax, UINT_MAX);
		c->pcb.rcv_wscale = 0;
	}
	if (!(opt_en & TCPOPT_MSS_ENBIT)) {
		c->pcb.snd_mss = tcp_calculate_mss(FLUX_FNET_MTU);
	}
	return opt_en;
}

/* slow path for handling ingress packets for TCP connections */
static void __tcp_rx_conn(tcp_conn_t *c, struct mbuf *m, uint32_t ack,
			  uint32_t snd_nxt, uint32_t win,
			  const unsigned char *optp, int optlen)
{
	struct list_head q = LIST_HEAD_INIT(q);
	struct list_head waiters = LIST_HEAD_INIT(waiters);
	struct mbuf *retransmit = NULL;
	uint32_t seq, len;
	bool do_ack = false, do_drop = true, fin = false, snd_was_full;
	bool ack_same = false, wnd_updated = false, wake = false;
	int ret;
	tcp_wait_queue_entry_t *wq_entry = NULL;
	struct tcp_options opts;

	seq = m->seg_seq;
	len = m->seg_end - m->seg_seq;

	if (unlikely((m->flags & TCP_FIN) > 0))
		len++;

	if (unlikely(c->pcb.state == TCP_STATE_CLOSED)) {
		if ((m->flags & TCP_RST) == 0)
			send_rst(c, false, seq, ack, len);
		goto done;
	}

	if (unlikely(c->pcb.state == TCP_STATE_SYN_SENT)) {
		if ((m->flags & TCP_ACK) > 0) {
			if (wraps_lte(ack, c->pcb.iss) ||
			    wraps_gt(ack, snd_nxt)) {
				send_rst(c, false, seq, ack, len);
				goto done;
			}
			if ((m->flags & TCP_RST) > 0) {
				/* check if the ack is valid */
				if (wraps_lte(c->pcb.snd_una, ack) &&
				    wraps_lte(ack, snd_nxt)) {
					tcp_conn_fail(c, ECONNRESET);
					goto done;
				}
			}
		} else if ((m->flags & TCP_RST) > 0) {
			goto done;
		}
		if ((m->flags & TCP_SYN) > 0) {
			c->pcb.rcv_nxt = seq + 1;
			c->pcb.irs = seq;

			/* set up options */
			opts.opt_en = __tcp_parse_options(c, optp, optlen);
			opts.mss = c->pcb.rcv_mss;
			opts.wscale = c->pcb.rcv_wscale;

			if ((m->flags & TCP_ACK) > 0) {
				c->pcb.snd_una = ack;
				tcp_conn_ack(c, &q);
			}
			if (wraps_gt(c->pcb.snd_una, c->pcb.iss)) {
				do_ack = true;
				c->pcb.snd_wnd = win;
				c->pcb.snd_wl1 = seq;
				c->pcb.snd_wl2 = ack;
				tcp_conn_set_state(c, TCP_STATE_ESTABLISHED);
			} else {
				ret = tcp_tx_ctl(c, TCP_SYN | TCP_ACK, &opts);
				if (unlikely(ret)) {
					goto done; /* feign packet loss */
				}
				tcp_conn_set_state(c, TCP_STATE_SYN_RECEIVED);
			}
		}
		goto done;
	}

	/*
	 * TCP_STATE_SYN_RECEIVED || TCP_STATE_ESTABLISHED ||
	 * TCP_STATE_FIN_WAIT1 || TCP_STATE_FIN_WAIT2 ||
	 * TCP_STATE_CLOSE_WAIT || TCP_STATE_CLOSING ||
	 * TCP_STATE_LAST_ACK || TCP_STATE_TIME_WAIT
	 */

	/* step 1 - acceptability testing */
	if (unlikely(!is_acceptable(c, len, seq, m->flags))) {
		do_ack = (m->flags & TCP_RST) == 0;
		goto done;
	}

	/* step 2 - RST */
	if (unlikely((m->flags & TCP_RST) > 0)) {
		tcp_conn_fail(c, ECONNRESET);
		goto done;
	}

	/* step 3 - security checks skipped */

	/* step 4 - SYN */
	if (unlikely((m->flags & TCP_SYN) > 0)) {
		send_rst(c, (m->flags & TCP_ACK) > 0, seq, ack, len);
		tcp_conn_fail(c, ECONNRESET);
		goto done;
	}

	/* step 5 - ACK */
	if (unlikely((m->flags & TCP_ACK) == 0)) {
		goto done;
	}
	if (unlikely(c->pcb.state == TCP_STATE_SYN_RECEIVED)) {
		if (!(wraps_lte(c->pcb.snd_una, ack) &&
		      wraps_lte(ack, snd_nxt))) {
			send_rst(c, true, seq, ack, len);
			do_drop = true;
			goto done;
		}
		fnet_dbg("conn established after %llx\n", (u64)m);
		c->pcb.snd_wnd = win;
		c->pcb.snd_wl1 = seq;
		c->pcb.snd_wl2 = ack;
		tcp_conn_set_state(c, TCP_STATE_ESTABLISHED);
	}

	/* process ack and window update */
	snd_was_full = tcp_is_snd_full(c);
	if (wraps_lte(c->pcb.snd_una, ack) && wraps_lte(ack, snd_nxt)) {
		/* did sent segments get acked? */
		if (c->pcb.snd_una != ack) {
			c->pcb.snd_una = ack;
			tcp_conn_ack(c, &q);
		} else {
			ack_same = true;
		}

		/* should we update the send window? */
		if (wraps_lt(c->pcb.snd_wl1, seq) ||
		    (c->pcb.snd_wl1 == seq && wraps_lte(c->pcb.snd_wl2, ack))) {
			if (!ack_same || c->pcb.snd_wnd <= win) {
				if (c->pcb.snd_wnd != win ||
				    c->pcb.snd_wl2 != ack)
					wnd_updated = true;
				c->pcb.snd_wnd = win;
				c->pcb.snd_wl1 = seq;
				c->pcb.snd_wl2 = ack;
			}
		}
	} else if (wraps_gt(ack, snd_nxt)) {
		do_ack = true;
		goto done;
	}
	if (snd_was_full && !tcp_is_snd_full(c)) {
		poll_set(&c->poll, POLLOUT);
		tcp_wait_queue_wake_up_all_start(&c->tx_wq, &waiters);
	}

	/*
	 * Fast retransmit -> detect a duplicate ACK if:
	 * 1. The ACK number is the same as the largest seen.
	 * 2. There is unacknowledged data pending.
	 * 3. There is no data payload included with the ACK.
	 * 4. There is no window update.
	 */
	if (unlikely(ack_same && c->pcb.snd_una != c->pcb.snd_nxt && len == 0 &&
		     !wnd_updated)) {
		c->rep_acks++;
		if (c->rep_acks >= TCP_FAST_RETRANSMIT_THRESH) {
			if (c->tx_exclusive) {
				c->do_fast_retransmit = true;
				c->fast_retransmit_last_ack = ack;
			} else {
				retransmit = tcp_tx_fast_retransmit_start(c);
			}
			c->rep_acks = 0;
		}
	} else if (c->pcb.snd_una == ack) {
		c->rep_acks = 0;
	}

	if (c->pcb.state == TCP_STATE_FIN_WAIT1 && c->pcb.snd_una == snd_nxt) {
		tcp_conn_set_state(c, TCP_STATE_FIN_WAIT2);
	} else if (c->pcb.state == TCP_STATE_CLOSING &&
		   c->pcb.snd_una == snd_nxt) {
		c->time_wait_ts = now_us();
		tcp_conn_set_state(c, TCP_STATE_TIME_WAIT);
	} else if (c->pcb.state == TCP_STATE_LAST_ACK &&
		   c->pcb.snd_una == snd_nxt) {
		tcp_conn_set_state(c, TCP_STATE_CLOSED);
		tcp_conn_put(c); /* safe because RCU + preempt is disabled */
		goto done;
	}

	/* step 6 - URG support skipped */

	/* step 7 - segment text */
	if (len > 0 && (c->pcb.state == TCP_STATE_ESTABLISHED ||
			c->pcb.state == TCP_STATE_FIN_WAIT1 ||
			c->pcb.state == TCP_STATE_FIN_WAIT2)) {
		m->seg_end = seq + len;

		do_drop = !tcp_rx_text(c, m, &wake, &fin);

		if (wake) {
			wq_entry = tcp_wait_queue_signal_start(&c->rx_wq);
			poll_set(&c->poll, POLLIN);
		}
		if (++c->acks_delayed_cnt >= 2) {
			do_ack = true;
		} else if (!c->ack_delayed) {
			c->ack_delayed = true;
			c->ack_ts = now_us();
		}
		do_ack |= !list_empty(&c->rxq_ooo);
	}

	/* step 8 - FIN */
	if (likely(!fin))
		goto done;

	if (c->pcb.state == TCP_STATE_ESTABLISHED) {
		tcp_conn_set_state(c, TCP_STATE_CLOSE_WAIT);
	} else if (c->pcb.state == TCP_STATE_FIN_WAIT1) {
		tcp_conn_set_state(c, TCP_STATE_CLOSING);
	} else if (c->pcb.state == TCP_STATE_FIN_WAIT2) {
		c->time_wait_ts = now_us();
		do_ack = true;
		tcp_conn_set_state(c, TCP_STATE_TIME_WAIT);
	}

done:
	tcp_timer_update(c);
	if (do_ack) {
		c->ack_delayed = false;
		c->acks_delayed_cnt = 0;
	}
	spin_unlock(&c->lock);

	/* deferred work (delayed until after the lock was dropped) */
	tcp_wait_queue_wake_up_all_finish(&waiters);
	if (wq_entry)
		tcp_wait_queue_signal_finish(wq_entry);
	mbuf_list_free(&q);
	tcp_tx_fast_retransmit_finish(c, retransmit);
	if (do_ack)
		tcp_tx_ack(c);
	if (do_drop)
		mbuf_free(m);
}

/* handles ingress packets for TCP listener queues */
tcp_conn_t *tcp_rx_listener(struct net_addr laddr, struct mbuf *m)
{
	struct net_addr raddr;
	const struct iphdr *iphdr;
	const struct tcphdr *tcphdr;
	const unsigned char *optp;
	tcp_conn_t *c;
	struct tcp_options opts;
	uint32_t hdr_len;
	int optlen, ret;

	/* find header offsets */
	iphdr = mbuf_network_hdr(m, *iphdr);
	tcphdr = mbuf_pull_hdr_or_null(m, *tcphdr);
	if (unlikely(!tcphdr))
		return NULL;

	/* calculate local and remote network addresses */
	raddr.ip = ntohl(iphdr->saddr);
	raddr.port = ntohs(tcphdr->source);

	/* do exactly what RFC 793 says */
	if ((tcp_flag_byte(tcphdr) & TCP_RST) > 0)
		return NULL;
	if ((tcp_flag_byte(tcphdr) & TCP_ACK) > 0) {
		tcp_tx_raw_rst(laddr, raddr, ntohl(tcphdr->ack_seq));
		return NULL;
	}
	if ((tcp_flag_byte(tcphdr) & TCP_SYN) == 0)
		return NULL;

	/* TODO: the spec requires us to enqueue but not post any data */
	hdr_len = tcphdr->doff * sizeof(uint32_t);
	if (ntohs(iphdr->tot_len) - sizeof(*iphdr) != hdr_len)
		return NULL;

	/* parse options */
	optlen = hdr_len - sizeof(struct tcphdr);
	optp = mbuf_pull_or_null(m, optlen);
	if (!optp)
		return NULL;

	/* we have a valid SYN packet, initialize a new connection */
	c = tcp_conn_alloc();
	if (unlikely(!c))
		return NULL;
	c->pcb.irs = ntohl(tcphdr->seq);
	c->pcb.rcv_nxt = c->pcb.irs + 1;

	/* set up options */
	opts.opt_en = __tcp_parse_options(c, optp, optlen);
	opts.mss = c->pcb.rcv_mss;
	opts.wscale = c->pcb.rcv_wscale;

	/*
	 * attach the connection to the transport layer. From this point onward
	 * ingress packets can be dispatched to the connection.
	 */
	ret = tcp_conn_attach(c, laddr, raddr);
	if (unlikely(ret)) {
		kfree(c);
		return NULL;
	}

	/* finally, send a SYN/ACK to the remote host */
	spin_lock(&c->lock);
	ret = tcp_tx_ctl(c, TCP_SYN | TCP_ACK, &opts);
	if (unlikely(ret)) {
		spin_unlock(&c->lock);
		tcp_conn_destroy(c);
		return NULL;
	}
	tcp_conn_get(c); /* take a ref for the state machine */
	tcp_conn_set_state(c, TCP_STATE_SYN_RECEIVED);
	spin_unlock(&c->lock);

	return c;
}

void tcp_rx_closed(struct mbuf *m)
{
	struct net_addr l, r;
	uint32_t len;
	const struct iphdr *iphdr;
	const struct tcphdr *tcphdr;

	iphdr = mbuf_network_hdr(m, *iphdr);
	tcphdr = mbuf_pull_hdr_or_null(m, *tcphdr);
	if (!tcphdr)
		return;

	if ((tcp_flag_byte(tcphdr) & TCP_RST) > 0)
		return;

	l.ip = ntohl(iphdr->daddr);
	l.port = ntohs(tcphdr->dest);

	r.ip = ntohl(iphdr->saddr);
	r.port = ntohs(tcphdr->source);

	if ((tcp_flag_byte(tcphdr) & TCP_ACK) > 0) {
		tcp_tx_raw_rst(l, r, ntohl(tcphdr->ack_seq));
	} else {
		len = ntohs(iphdr->tot_len) - sizeof(*iphdr) - tcphdr->doff * 4;
		tcp_tx_raw_rst_ack(l, r, 0, ntohl(tcphdr->seq) + len);
	}
}
