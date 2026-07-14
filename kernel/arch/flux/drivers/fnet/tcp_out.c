/*
 * tcp_out.c - the egress datapath for TCP
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <net/ip.h>
#include <net/tcp.h>
#include <net/tcp_states.h>
#include <uapi/linux/tcp.h>

#include "fnet.h"
#include "core.h"
#include "tcp.h"
#include "csum.h"

static void tcp_tx_release_mbuf(struct mbuf *m)
{
	if (atomic_dec_and_test(&m->ref))
		net_tx_release_mbuf(m);
}

static inline size_t tcp_headroom(void)
{
	return ip_headroom() + sizeof(struct tcphdr);
}

static void __tcp_hdr_chksum(struct tcphdr *hdr, uint32_t local_ip,
			     uint32_t remote_ip, uint16_t len)
{
	len += hdr->doff * sizeof(uint32_t);

	if (!fnet_dev->arg.csum_offload) {
		hdr->check =
			ipv4_phdr_cksum(IPPROTO_TCP, local_ip, remote_ip, len);
		return;
	}

	hdr->check = 0;
	hdr->check =
		ipv4_udptcp_cksum(IPPROTO_TCP, local_ip, remote_ip, len, hdr);
}

static __always_inline void tcp_hdr_chksum(struct tcphdr *hdr,
					   uint32_t local_ip,
					   uint32_t remote_ip, uint16_t len)
{
	__tcp_hdr_chksum(hdr, local_ip, remote_ip, len);
}

static __always_inline struct tcphdr *
__tcp_add_tcphdr(struct mbuf *m, tcp_conn_t *c, uint8_t flags, uint8_t off,
		 uint16_t l4len, bool push)
{
	struct tcphdr *tcphdr;
	uint64_t rcv_nxt_wnd = load_acquire(&c->pcb.rcv_nxt_wnd);
	tcp_seq ack = c->tx_last_ack = (uint32_t)rcv_nxt_wnd;
	uint32_t win = c->tx_last_win = rcv_nxt_wnd >> 32;

	/* write the tcp header */
	if (push)
		tcphdr = mbuf_push_hdr(m, *tcphdr);
	else
		tcphdr = mbuf_put_hdr(m, *tcphdr);
	mbuf_mark_transport_offset(m);
	tcphdr->source = htons(c->e.laddr.port);
	tcphdr->dest = htons(c->e.raddr.port);
	tcphdr->ack_seq = htonl(ack);
	tcphdr->doff = off;
	tcp_flag_byte(tcphdr) = flags;
	tcphdr->window = htons(win >> c->pcb.rcv_wscale);
	tcphdr->seq = htonl(m->seg_seq);
	tcp_hdr_chksum(tcphdr, c->e.laddr.ip, c->e.raddr.ip, l4len);

	mbuf_mark_l4_ports(m, c->e.laddr.port, c->e.raddr.port);

	return tcphdr;
}

static __always_inline struct tcphdr *
tcp_push_tcphdr(struct mbuf *m, tcp_conn_t *c, uint8_t flags, uint8_t off,
		uint16_t l4len)
{
	return __tcp_add_tcphdr(m, c, flags, off, l4len, true);
}

/**
 * tcp_tx_raw_rst - send a RST without an established connection
 * @laddr: the local address
 * @raddr: the remote address
 * @seq: the segement's sequence number
 *
 * Returns 0 if successful, otherwise fail.
 */
int tcp_tx_raw_rst(struct net_addr laddr, struct net_addr raddr, tcp_seq seq)
{
	struct tcphdr *tcphdr;
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf(tcp_headroom());
	if (unlikely((!m)))
		return -ENOMEM;

	fnet_dbg("sending RST from %pI4:%u to %pI4:%u in 0x%llx\n", &laddr.ip,
		 laddr.port, &raddr.ip, raddr.port, (u64)m);

	m->txflags = FLUX_OLFLAG_L3_CHKSUM;

	/* write the tcp header */
	tcphdr = mbuf_push_hdr(m, *tcphdr);
	tcphdr->source = htons(laddr.port);
	tcphdr->dest = htons(raddr.port);
	tcphdr->seq = htonl(seq);
	tcphdr->ack_seq = htonl(0);
	tcphdr->doff = 5;
	tcp_flag_byte(tcphdr) = TCP_RST;
	tcphdr->window = htons(0);
	tcp_hdr_chksum(tcphdr, laddr.ip, raddr.ip, 0);

	mbuf_mark_l4_ports(m, laddr.port, raddr.port);

	/* transmit packet */
	ret = net_tx_ip(m, IPPROTO_TCP, raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_raw_rst_ack - send a RST/ACK without an established connection
 * @laddr: the local address
 * @raddr: the remote address
 * @seq: the segment's sequence number
 * @ack: the segment's acknowledgement number
 *
 * Returns 0 if successful, otherwise fail.
 */
int tcp_tx_raw_rst_ack(struct net_addr laddr, struct net_addr raddr,
		       tcp_seq seq, tcp_seq ack)
{
	struct tcphdr *tcphdr;
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf(tcp_headroom());
	if (unlikely((!m)))
		return -ENOMEM;

	fnet_dbg("sending RST from %pI4:%u to %pI4:%u in 0x%llx\n", &laddr.ip,
		 laddr.port, &raddr.ip, raddr.port, (u64)m);

	m->txflags = FLUX_OLFLAG_L3_CHKSUM;

	/* write the tcp header */
	tcphdr = mbuf_push_hdr(m, *tcphdr);
	tcphdr->source = htons(laddr.port);
	tcphdr->dest = htons(raddr.port);
	tcphdr->seq = htonl(seq);
	tcphdr->ack_seq = htonl(ack);
	tcphdr->doff = 5;
	tcp_flag_byte(tcphdr) = TCP_RST | TCP_ACK;
	tcphdr->window = htons(0);
	tcp_hdr_chksum(tcphdr, laddr.ip, raddr.ip, 0);

	mbuf_mark_l4_ports(m, laddr.port, raddr.port);

	/* transmit packet */
	ret = net_tx_ip(m, IPPROTO_TCP, raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_ack - send an acknowledgement and window update packet
 * @c: the connection to send the ACK
 *
 * Returns 0 if succesful, otherwise fail.
 */
int tcp_tx_ack(tcp_conn_t *c)
{
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf(tcp_headroom());
	if (unlikely(!m))
		return -ENOMEM;

	fnet_dbg("sending ACK in 0x%llx\n", (u64)m);

	m->txflags = FLUX_OLFLAG_L3_CHKSUM;
	m->seg_seq = load_acquire(&c->pcb.snd_nxt);
	tcp_push_tcphdr(m, c, TCP_ACK, 5, 0);

	/* transmit packet */
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_probe_window - send a packet to probe the window size
 * @c: the connection to probe
 *
 * Deliberately use a sequence that has already been acked. The receiver
 * will find it fails acceptability testing, and immediately send back an
 * ack with the latest window.
 *
 * Returns 0 if succesful, otherwise fail.
 */
int tcp_tx_probe_window(tcp_conn_t *c)
{
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf(tcp_headroom());
	if (unlikely(!m))
		return -ENOMEM;

	m->txflags = FLUX_OLFLAG_L3_CHKSUM;
	m->seg_seq = load_acquire(&c->pcb.snd_una) - 1;
	tcp_push_tcphdr(m, c, TCP_ACK, 5, 0);

	fnet_dbg("sending window probe in 0x%llx\n", (u64)m);

	/* transmit packet */
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

static int tcp_put_options(struct mbuf *m, const struct tcp_options *opts)
{
	uint32_t *ptr;
	int len = 0;

	/* WARNING: the order matters, as some devices are broken */

	if (opts->opt_en & TCPOPT_WINDOW_ENBIT) {
		ptr = (uint32_t *)mbuf_put(m, sizeof(uint32_t));
		*ptr = htonl((TCPOPT_NOP << 24) | (TCPOPT_WINDOW << 16) |
			     (TCPOLEN_WINDOW << 8) | opts->wscale);
		len++;
	}
	if (opts->opt_en & TCPOPT_MSS_ENBIT) {
		ptr = (uint32_t *)mbuf_put(m, sizeof(uint32_t));
		*ptr = htonl((TCPOPT_MSS << 24) | (TCPOLEN_MSS << 16) |
			     opts->mss);
		len++;
	}

	return len;
}

/**
 * tcp_tx_ctl - sends a control message without data
 * @c: the TCP connection
 * @flags: the control flags (e.g. TCP_SYN, TCP_FIN, etc.)
 * @opts: TCP options to include
 *
 * WARNING: The caller must have write exclusive access to the socket or hold
 * @c->lock while write exclusion isn't taken.
 *
 * Returns 0 if successful, -ENOMEM if out memory.
 */
int tcp_tx_ctl(tcp_conn_t *c, uint8_t flags, const struct tcp_options *opts)
{
	struct mbuf *m;
	int ret = 0;

	m = net_tx_alloc_mbuf(tcp_headroom());
	if (unlikely(!m))
		return -ENOMEM;

	m->txflags = FLUX_OLFLAG_L3_CHKSUM;
	m->seg_seq = c->pcb.snd_nxt;
	m->seg_end = c->pcb.snd_nxt + 1;
	m->flags = flags;

	fnet_dbg("sending ctl 0x%02x in 0x%llx\n", flags, (u64)m);

	if (opts)
		ret = tcp_put_options(m, opts);
	tcp_push_tcphdr(m, c, flags, 5 + ret, 0);
	store_release(&c->pcb.snd_nxt, c->pcb.snd_nxt + 1);
	list_add_tail(&m->link, &c->txq);
	m->timestamp = now_us();
	atomic_set(&m->ref, 2);
	m->release = tcp_tx_release_mbuf;
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret)) {
		/* pretend the packet was sent */
		atomic_set(&m->ref, 1);
	}
	return ret;
}

/**
 * tcp_tx_send - transmit a buffer on a TCP connection
 * @c: the TCP connection
 * @buf: the buffer to transmit
 * @len: the length of the buffer to transmit
 * @push: indicates the data is ready for consumption by the receiver
 *
 * If @push is false, the implementation may buffer some or all of the data for
 * future transmission.
 *
 * WARNING: The caller is responsible for respecting the TCP window size limit.
 * WARNING: The caller must have write exclusive access to the socket or hold
 * @c->lock while write exclusion isn't taken.
 *
 * Returns the number of bytes transmitted, or < 0 if there was an error.
 */
ssize_t tcp_tx_send(tcp_conn_t *c, const void *buf, size_t len, bool push)
{
	struct mbuf *m;
	const char *pos = buf;
	const char *end = pos + len;
	ssize_t ret = 0;
	size_t seglen, bufsz;
	uint32_t mss = c->pcb.snd_mss;

	pos = buf;
	end = pos + len;

	/* the main TCP segmenter loop */
	do {
		/* allocate a buffer and copy payload data */
		if (c->tx_pending) {
			m = c->tx_pending;
			c->tx_pending = NULL;
			seglen = min((uint32_t)(end - pos),
				     mss - mbuf_length(m));
			m->seg_end += seglen;
		} else {
			seglen = min((uint32_t)(end - pos), mss);
			if (push && pos + seglen == end)
				bufsz = seglen;
			else
				bufsz = mss;
			m = net_tx_alloc_mbuf(tcp_headroom());
			if (unlikely(!m)) {
				ret = -ENOBUFS;
				break;
			}
			m->seg_seq = c->pcb.snd_nxt;
			m->seg_end = c->pcb.snd_nxt + seglen;
			m->flags = TCP_ACK;
			atomic_set(&m->ref, 2);
			m->release = tcp_tx_release_mbuf;
		}

		memcpy(mbuf_put(m, seglen), pos, seglen);
		store_release(&c->pcb.snd_nxt, c->pcb.snd_nxt + seglen);
		pos += seglen;

		/* if not pushing, keep the last buffer for later */
		if (!push && pos == end &&
		    mbuf_length(m) - sizeof(struct tcphdr) < mss) {
			c->tx_pending = m;
			break;
		}

		/* initialize TCP header */
		if (push && pos == end)
			m->flags |= TCP_PUSH;
		tcp_push_tcphdr(m, c, m->flags, 5, m->seg_end - m->seg_seq);

		/* transmit the packet */
		list_add_tail(&m->link, &c->txq);
		m->timestamp = now_us();
		m->txflags = FLUX_OLFLAG_L3_CHKSUM;
		ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
		if (unlikely(ret)) {
			/* pretend the packet was sent */
			atomic_set(&m->ref, 1);
		}
	} while (pos < end);

	/* if we sent anything return the length we sent instead of an error */
	if (pos - (const char *)buf > 0)
		ret = pos - (const char *)buf;
	return ret;
}

static int tcp_tx_retransmit_one(tcp_conn_t *c, struct mbuf *m)
{
	int ret;
	uint8_t opts_len;
	uint16_t l4len;
	size_t sz;
	struct mbuf *newm;

	l4len = m->seg_end - m->seg_seq;
	if (m->flags & (TCP_SYN | TCP_FIN))
		l4len--;

	opts_len = ((struct tcphdr *)mbuf_transport_offset(m))->doff - 5;

	/*
	 * Check if still transmitting. Because of a limitation in some NIC
	 * drivers, completions could be delayed long after transmission is
	 * finished. We copy the packet to allow retransmission to still succeed
	 * in such corner cases.
	 */
	if (unlikely(atomic_read(&m->ref) != 1)) {
		sz = sizeof(uint32_t) * opts_len + l4len;
		newm = net_tx_alloc_mbuf(tcp_headroom());
		if (unlikely(!newm))
			return -ENOMEM;
		fnet_dbg("retransmitting packet by copy in 0x%llx\n",
			 (u64)newm);
		memcpy(mbuf_put(newm, sz),
		       mbuf_transport_offset(m) + sizeof(struct tcphdr), sz);
		newm->flags = m->flags;
		newm->seg_seq = m->seg_seq;
		newm->seg_end = m->seg_end;
		newm->txflags = FLUX_OLFLAG_L3_CHKSUM;
		m = newm;
	} else {
		fnet_dbg("retransmitting packet in 0x%llx\n", (u64)m);
		/* strip headers and reset ref count */
		mbuf_reset(m, m->transport_off + sizeof(struct tcphdr));
		atomic_set(&m->ref, 2);
	}

	/* handle a partially acknowledged packet */
	uint32_t una = load_acquire(&c->pcb.snd_una);
	if (unlikely(wraps_lte(m->seg_end, una))) {
		mbuf_free(m);
		return 0;
	} else if (unlikely(wraps_lt(m->seg_seq, una))) {
		mbuf_pull(m, una - m->seg_seq);
		m->seg_seq = una;
	}

	/* push the TCP header back on (now with fresher ack) */
	tcp_push_tcphdr(m, c, m->flags, 5 + opts_len, l4len);

	/* transmit the packet */
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_fast_retransmit - resend the first pending egress packet
 * @c: the TCP connection in which to send retransmissions
 */
struct mbuf *tcp_tx_fast_retransmit_start(tcp_conn_t *c)
{
	struct mbuf *m;

	if (c->tx_exclusive)
		return NULL;

	m = list_first_entry_or_null(&c->txq, struct mbuf, link);
	if (m) {
		m->timestamp = now_us();
		atomic_inc(&m->ref);
	}

	return m;
}

void tcp_tx_fast_retransmit_finish(tcp_conn_t *c, struct mbuf *m)
{
	if (m) {
		tcp_tx_retransmit_one(c, m);
		mbuf_free(m);
	}
}

/**
 * tcp_tx_retransmit - resend any pending egress packets that timed out
 * @c: the TCP connection in which to send retransmissions
 */
void tcp_tx_retransmit(tcp_conn_t *c)
{
	struct mbuf *m, *tmp;
	uint64_t now = now_us();
	int ret, count = 0;

	list_for_each_entry_safe(m, tmp, &c->txq, link) {
		/* check if the timeout expired */
		if (now - m->timestamp < TCP_RETRANSMIT_TIMEOUT)
			break;

		if (wraps_gte(load_acquire(&c->pcb.snd_una), m->seg_end))
			continue;

		m->timestamp = now;
		ret = tcp_tx_retransmit_one(c, m);
		if (ret)
			break;

		if (++count >= TCP_RETRANSMIT_BATCH)
			break;
	}
}
