#ifndef _ASM_UAPI_FLUX_OCI_H
#define _ASM_UAPI_FLUX_OCI_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

enum flux_signal_ctrl_op {
	FLUX_SIGNAL_CTRL_NONE = 0,
	FLUX_SIGNAL_CTRL_KILL = 1,
	FLUX_SIGNAL_CTRL_EXEC = 2,
};

#define FLUX_SIGNAL_CTRL_OP_MASK 0xffu
#define FLUX_SIGNAL_CTRL_SIGNO_SHIFT 8
#define FLUX_SIGNAL_CTRL_SIGNO_MASK 0xffu
#define FLUX_SIGNAL_CTRL_ARG_SHIFT 16
#define FLUX_SIGNAL_CTRL_ARG_MASK 0xffffu

static inline unsigned int
flux_signal_ctrl_pack(unsigned int op, unsigned int signo, unsigned int arg)
{
	return ((op & FLUX_SIGNAL_CTRL_OP_MASK) |
		((signo & FLUX_SIGNAL_CTRL_SIGNO_MASK)
		 << FLUX_SIGNAL_CTRL_SIGNO_SHIFT) |
		((arg & FLUX_SIGNAL_CTRL_ARG_MASK)
		 << FLUX_SIGNAL_CTRL_ARG_SHIFT));
}

static inline unsigned int flux_signal_ctrl_op(unsigned int value)
{
	return value & FLUX_SIGNAL_CTRL_OP_MASK;
}

static inline unsigned int flux_signal_ctrl_signo(unsigned int value)
{
	return (value >> FLUX_SIGNAL_CTRL_SIGNO_SHIFT) &
	       FLUX_SIGNAL_CTRL_SIGNO_MASK;
}

static inline unsigned int flux_signal_ctrl_arg(unsigned int value)
{
	return (value >> FLUX_SIGNAL_CTRL_ARG_SHIFT) &
	       FLUX_SIGNAL_CTRL_ARG_MASK;
}

#define FLUX_EXEC_RING_SLOTS 8U
#define FLUX_EXEC_INLINE_BYTES 3072U
#define FLUX_EXEC_RING_MAP_ADDR 0x380000000000ULL

enum flux_exec_slot_state {
	FLUX_EXEC_SLOT_FREE = 0,
	FLUX_EXEC_SLOT_READY = 1,
	FLUX_EXEC_SLOT_RUNNING = 2,
	FLUX_EXEC_SLOT_DONE = 3,
	FLUX_EXEC_SLOT_ERROR = 4,
};

enum flux_exec_request_flags {
	FLUX_EXEC_F_DETACH = 1U << 0,
	FLUX_EXEC_F_HAS_CWD = 1U << 1,
	FLUX_EXEC_F_HAS_ENV = 1U << 2,
	FLUX_EXEC_F_HAS_USER = 1U << 3,
};

enum flux_exec_session_id {
	FLUX_EXEC_SESSION_NONE = 0,
	FLUX_EXEC_SESSION_DYNAMIC_STDIO_BASE = 0x10000000u,
	FLUX_EXEC_SESSION_DYNAMIC_TTY_BASE = 0x20000000u,
};

/*
 * exec requests are written by the host-side runtime and consumed by the
 * kernel-side exec worker. `head` is the next slot the consumer will inspect,
 * `tail` is the next slot the producer may publish, and `next_seq` gives each
 * published request a monotonically increasing identity for doorbell payloads
 * and result correlation.
 */
struct flux_exec_ring_hdr {
	uint32_t head;
	uint32_t tail;
	uint32_t size;
	uint32_t reserved;
	uint64_t next_seq;
};

/*
 * A producer fills one slot, transitions it from FREE to READY, and then
 * raises SIGUSR1(op=EXEC, arg=...). The consumer transitions READY to RUNNING
 * and finally to DONE or ERROR after the request has completed.
 */
struct flux_exec_slot {
	uint32_t state;
	uint32_t flags;
	uint64_t seq;
	int32_t status;
	uint32_t argc;
	uint32_t envc;
	uint32_t uid;
	uint32_t gid;
	uint32_t session_id;
	uint32_t filename_off;
	uint32_t cwd_off;
	uint32_t argv_off;
	uint32_t env_off;
	uint32_t data_len;
	uint8_t data[FLUX_EXEC_INLINE_BYTES];
};

struct flux_exec_ring {
	struct flux_exec_ring_hdr hdr;
	struct flux_exec_slot slots[FLUX_EXEC_RING_SLOTS];
};

static inline uint32_t flux_exec_slot_state_load(const struct flux_exec_slot *slot)
{
	return !slot ? FLUX_EXEC_SLOT_FREE :
		       __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
}

static inline void flux_exec_slot_state_store(struct flux_exec_slot *slot,
					      uint32_t state)
{
	if (!slot)
		return;

	__atomic_store_n(&slot->state, state, __ATOMIC_RELEASE);
}

static inline uint32_t flux_exec_ring_tail_load(const struct flux_exec_ring *ring)
{
	return !ring ? 0 : __atomic_load_n(&ring->hdr.tail, __ATOMIC_ACQUIRE);
}

static inline void flux_exec_ring_tail_store(struct flux_exec_ring *ring,
					     uint32_t tail)
{
	if (!ring)
		return;

	__atomic_store_n(&ring->hdr.tail, tail, __ATOMIC_RELEASE);
}

static inline uint32_t flux_exec_ring_next_index(const struct flux_exec_ring *ring,
						 uint32_t index)
{
	if (!ring || ring->hdr.size == 0)
		return 0;

	index++;
	if (index >= ring->hdr.size)
		index = 0;
	return index;
}

static inline struct flux_exec_slot *
flux_exec_ring_slot(struct flux_exec_ring *ring, uint32_t index)
{
	if (!ring || index >= ring->hdr.size)
		return NULL;

	return &ring->slots[index];
}

static inline const struct flux_exec_slot *
flux_exec_ring_slot_const(const struct flux_exec_ring *ring, uint32_t index)
{
	if (!ring || index >= ring->hdr.size)
		return NULL;

	return &ring->slots[index];
}

static inline int flux_exec_ring_is_empty(const struct flux_exec_ring *ring)
{
	return !ring || ring->hdr.head == flux_exec_ring_tail_load(ring);
}

static inline int flux_exec_ring_is_full(const struct flux_exec_ring *ring)
{
	return ring &&
	       flux_exec_ring_next_index(ring, ring->hdr.tail) == ring->hdr.head;
}

#endif /* _ASM_UAPI_FLUX_OCI_H */
