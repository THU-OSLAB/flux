#ifndef _UTILS_LRPC_H
#define _UTILS_LRPC_H

#include <utils/base.h>

/**
 * store_release - store a native value with release fence semantics
 * @p: the pointer to store
 * @v: the value to store
 */
#define store_release(p, v)          \
	do {                         \
		barrier();           \
		ACCESS_ONCE(*p) = v; \
	} while (0)

/**
 * load_acquire - load a native value with acquire fence semantics
 * @p: the pointer to load
 */
#define load_acquire(p)                           \
	({                                        \
		typeof(*p) __p = ACCESS_ONCE(*p); \
		barrier();                        \
		__p;                              \
	})

struct lrpc_msg {
	uint64_t cmd;
	unsigned long payload;
};

#define LRPC_DONE_PARITY (1UL << 63)
#define LRPC_CMD_MASK (~LRPC_DONE_PARITY)

/* Producer-side shared-memory channel state. */
struct lrpc_chan_out {
	uint32_t send_head;
	uint32_t send_tail;
	struct lrpc_msg *tbl;
	uint32_t *recv_head_wb;
	uint32_t size;
	uint32_t pad;
};

extern bool __lrpc_send(struct lrpc_chan_out *chan, uint64_t cmd,
			unsigned long payload);

/*
 * Queue a message onto an outbound LRPC channel.
 *
 * Returns false if the channel is full.
 */
static inline bool lrpc_send(struct lrpc_chan_out *chan, uint64_t cmd,
			     unsigned long payload)
{
	struct lrpc_msg *dst;

	assert(!(cmd & LRPC_DONE_PARITY));

	if (unlikely(chan->send_head - chan->send_tail >= chan->size))
		return __lrpc_send(chan, cmd, payload);

	dst = &chan->tbl[chan->send_head & (chan->size - 1)];
	cmd |= (chan->send_head++ & chan->size) ? 0 : LRPC_DONE_PARITY;
	dst->payload = payload;
	store_release(&dst->cmd, cmd);
	return true;
}

/* Return the last cached number of free slots in an outbound channel. */
static inline uint32_t lrpc_get_cached_send_window(struct lrpc_chan_out *chan)
{
	return chan->size - chan->send_head + chan->send_tail;
}

/* Return the last cached number of queued outbound messages. */
static inline uint32_t lrpc_get_cached_length(struct lrpc_chan_out *chan)
{
	return chan->send_head - chan->send_tail;
}

/* Refresh and return the producer's observed send tail. */
static inline uint32_t lrpc_poll_send_tail(struct lrpc_chan_out *chan)
{
	chan->send_tail = load_acquire(chan->recv_head_wb);
	return chan->send_tail;
}

extern int lrpc_init_out(struct lrpc_chan_out *chan, struct lrpc_msg *tbl,
			 unsigned int size, uint32_t *recv_head_wb);

/* Consumer-side shared-memory channel state. */
struct lrpc_chan_in {
	struct lrpc_msg *tbl;
	uint32_t *recv_head_wb;
	uint32_t recv_head;
	uint32_t size;
};

/*
 * Receive a message from an inbound LRPC channel.
 *
 * Returns false if the channel is empty.
 */
static inline bool lrpc_recv(struct lrpc_chan_in *chan, uint64_t *cmd_out,
			     unsigned long *payload_out)
{
	struct lrpc_msg *m = &chan->tbl[chan->recv_head & (chan->size - 1)];
	uint64_t parity = (chan->recv_head & chan->size) ? 0 : LRPC_DONE_PARITY;
	uint64_t cmd;

	cmd = load_acquire(&m->cmd);
	if ((cmd & LRPC_DONE_PARITY) != parity)
		return false;
	chan->recv_head++;

	*cmd_out = cmd & LRPC_CMD_MASK;
	*payload_out = m->payload;
	store_release(chan->recv_head_wb, chan->recv_head);
	return true;
}

/* Return true when no inbound messages are visible to the consumer. */
static inline bool lrpc_empty(struct lrpc_chan_in *chan)
{
	struct lrpc_msg *m = &chan->tbl[chan->recv_head & (chan->size - 1)];
	uint64_t parity = (chan->recv_head & chan->size) ? 0 : LRPC_DONE_PARITY;

	return (ACCESS_ONCE(m->cmd) & LRPC_DONE_PARITY) != parity;
}

extern int lrpc_init_in(struct lrpc_chan_in *chan, struct lrpc_msg *tbl,
			unsigned int size, uint32_t *recv_head_wb);

#endif /* _UTILS_LRPC_H */
