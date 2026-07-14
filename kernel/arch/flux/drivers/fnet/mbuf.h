/* Imported rte_mbuf layout definitions */
#ifndef _FLUX_IOK_NET_MBUF_H
#define _FLUX_IOK_NET_MBUF_H

#include <linux/types.h>
#include <linux/list.h>
#include <asm/bug.h>

/** 
 * HACK: These macros depend on the imported rte_mbuf layout.
 */

/**
 * Mbuf having an external buffer attached. shinfo in mbuf must be filled.
 */
#define RTE_MBUF_F_EXTERNAL (1ULL << 61)

typedef uint64_t rte_iova_t;

struct rte_mbuf_sched {
	uint32_t queue_id; /**< Queue ID. */
	uint8_t traffic_class;
	/**< Traffic class ID. Traffic class 0
	 * is the highest priority traffic class.
	 */
	uint8_t color;
	/**< Color. @see enum rte_color.*/
	uint16_t reserved; /**< Reserved. */
}; /**< Hierarchical scheduler */

enum {
	RTE_MBUF_L2_LEN_BITS = 7,
	RTE_MBUF_L3_LEN_BITS = 9,
	RTE_MBUF_L4_LEN_BITS = 8,
	RTE_MBUF_TSO_SEGSZ_BITS = 16,
	RTE_MBUF_OUTL3_LEN_BITS = 9,
	RTE_MBUF_OUTL2_LEN_BITS = 7,
};

/**
 * The generic rte_mbuf, containing a packet mbuf.
 */
struct rte_mbuf {
	void *buf_addr; /**< Virtual address of segment buffer. */
	/**
	 * Physical address of segment buffer.
	 * This field is undefined if the build is configured to use only
	 * virtual address as IOVA (i.e. RTE_IOVA_IN_MBUF is 0).
	 * Force alignment to 8-bytes, so as to ensure we have the exact
	 * same mbuf cacheline0 layout for 32-bit and 64-bit. This makes
	 * working on vector drivers easier.
	 */
	rte_iova_t buf_iova;
	uint16_t data_off;

	/**
	 * Reference counter. Its size should at least equal to the size
	 * of port field (16 bits), to support zero-copy broadcast.
	 * It should only be accessed using the following functions:
	 * rte_mbuf_refcnt_update(), rte_mbuf_refcnt_read(), and
	 * rte_mbuf_refcnt_set(). The functionality of these functions (atomic,
	 * or non-atomic) is controlled by the RTE_MBUF_REFCNT_ATOMIC flag.
	 */
	uint16_t refcnt;

	/**
	 * Number of segments. Only valid for the first segment of an mbuf
	 * chain.
	 */
	uint16_t nb_segs;

	/** Input port (16 bits to support more than 256 virtual ports).
	 * The event eth Tx adapter uses this field to specify the output port.
	 */
	uint16_t port;

	uint64_t ol_flags; /**< Offload features. */

	/*
	 * The packet type, which is the combination of outer/inner L2, L3, L4
	 * and tunnel types. The packet_type is about data really present in the
	 * mbuf. Example: if vlan stripping is enabled, a received vlan packet
	 * would have RTE_PTYPE_L2_ETHER and not RTE_PTYPE_L2_VLAN because the
	 * vlan is stripped from the data.
	 */
	union {
		uint32_t packet_type; /**< L2/L3/L4 and tunnel information. */
		__extension__ struct {
			uint8_t l2_type : 4; /**< (Outer) L2 type. */
			uint8_t l3_type : 4; /**< (Outer) L3 type. */
			uint8_t l4_type : 4; /**< (Outer) L4 type. */
			uint8_t tun_type : 4; /**< Tunnel type. */
			union {
				uint8_t inner_esp_next_proto;
				__extension__ struct {
					uint8_t inner_l2_type : 4;
					uint8_t inner_l3_type : 4;
				};
			};
			uint8_t inner_l4_type : 4; /**< Inner L4 type. */
		};
	};
	uint32_t pkt_len; /**< Total pkt len: sum of all segments. */
	uint16_t data_len; /**< Amount of data in segment buffer. */
	uint16_t vlan_tci;

	union {
		union {
			uint32_t rss; /**< RSS hash result if RSS enabled */
			struct {
				union {
					struct {
						uint16_t hash;
						uint16_t id;
					};
					uint32_t lo;
					/**< Second 4 flexible bytes */
				};
				uint32_t hi;
				/**< First 4 flexible bytes or FD ID, dependent
				 * on RTE_MBUF_F_RX_FDIR_* flag in ol_flags.
				 */
			} fdir; /**< Filter identifier if FDIR enabled */
			struct rte_mbuf_sched sched;
			/**< Hierarchical scheduler : 8 bytes */
			struct {
				uint32_t reserved1;
				uint16_t reserved2;
				uint16_t txq;
				/**< The event eth Tx adapter uses this field
				 * to store Tx queue id.
				 * @see rte_event_eth_tx_adapter_txq_set()
				 */
			} txadapter; /**< Eventdev ethdev Tx adapter */
			uint32_t usr;
			/**< User defined tags. See rte_distributor_process() */
		} hash; /**< hash information */
	};

	/** Outer VLAN TCI (CPU order), valid if RTE_MBUF_F_RX_QINQ is set. */
	uint16_t vlan_tci_outer;

	uint16_t buf_len; /**< Length of segment buffer. */

	struct rte_mempool *pool; /**< Pool from which mbuf was allocated. */

	/**
	 * Next segment of scattered packet. Must be NULL in the last
	 * segment or in case of non-segmented packet.
	 */
	struct rte_mbuf *next;

	/* fields to support TX offloads */
	union {
		uint64_t tx_offload; /**< combined for easy fetch */
		__extension__ struct {
			uint64_t l2_len : RTE_MBUF_L2_LEN_BITS;
			/**< L2 (MAC) Header Length for non-tunneling pkt.
			 * Outer_L4_len + ... + Inner_L2_len for tunneling pkt.
			 */
			uint64_t l3_len : RTE_MBUF_L3_LEN_BITS;
			/**< L3 (IP) Header Length. */
			uint64_t l4_len : RTE_MBUF_L4_LEN_BITS;
			/**< L4 (TCP/UDP) Header Length. */
			uint64_t tso_segsz : RTE_MBUF_TSO_SEGSZ_BITS;
			/**< TCP TSO segment size */

			/*
			 * Fields for Tx offloading of tunnels.
			 * These are undefined for packets which don't request
			 * any tunnel offloads (outer IP or UDP checksum,
			 * tunnel TSO).
			 *
			 * PMDs should not use these fields unconditionally
			 * when calculating offsets.
			 *
			 * Applications are expected to set appropriate tunnel
			 * offload flags when they fill in these fields.
			 */
			uint64_t outer_l3_len : RTE_MBUF_OUTL3_LEN_BITS;
			/**< Outer L3 (IP) Hdr Length. */
			uint64_t outer_l2_len : RTE_MBUF_OUTL2_LEN_BITS;
			/**< Outer L2 (MAC) Hdr Length. */

			/* uint64_t unused:RTE_MBUF_TXOFLD_UNUSED_BITS; */
		};
	};

	/** Shared data for external buffer attached to mbuf. See
	 * rte_pktmbuf_attach_extbuf().
	 */
	struct rte_mbuf_ext_shared_info *shinfo;

	/** Size of the application private data. In case of an indirect
	 * mbuf, it stores the direct mbuf private data size.
	 */
	uint16_t priv_size;

	/** Timesync flags for use with IEEE1588. */
	uint16_t timesync;

	uint32_t dynfield1[9]; /**< Reserved for dynamic fields. */
};

/**
 * TCP segmentation offload. To enable this offload feature for a
 * packet to be transmitted on hardware supporting TSO:
 *  - set the RTE_MBUF_F_TX_TCP_SEG flag in mbuf->ol_flags (this flag implies
 *    RTE_MBUF_F_TX_TCP_CKSUM)
 *  - set the flag RTE_MBUF_F_TX_IPV4 or RTE_MBUF_F_TX_IPV6
 *  - if it's IPv4, set the RTE_MBUF_F_TX_IP_CKSUM flag
 *  - fill the mbuf offload information: l2_len, l3_len, l4_len, tso_segsz
 */
#define RTE_MBUF_F_TX_TCP_SEG (1ULL << 50)

/** TCP cksum of TX pkt. computed by NIC. */
#define RTE_MBUF_F_TX_TCP_CKSUM (1ULL << 52)

/** UDP cksum of TX pkt. computed by NIC. */
#define RTE_MBUF_F_TX_UDP_CKSUM (3ULL << 52)

/**
 * Offload the IP checksum in the hardware. The flag RTE_MBUF_F_TX_IPV4 should
 * also be set by the application, although a PMD will only check
 * RTE_MBUF_F_TX_IP_CKSUM.
 *  - fill the mbuf offload information: l2_len, l3_len
 */
#define RTE_MBUF_F_TX_IP_CKSUM (1ULL << 54)

/**
 * Packet is IPv4. This flag must be set when using any offload feature
 * (TSO, L3 or L4 checksum) to tell the NIC that the packet is an IPv4
 * packet. If the packet is a tunneled packet, this flag is related to
 * the inner headers.
 */
#define RTE_MBUF_F_TX_IPV4 (1ULL << 55)

/**
 * Packet is IPv6. This flag must be set when using an offload feature
 * (TSO or L4 checksum) to tell the NIC that the packet is an IPv6
 * packet. If the packet is a tunneled packet, this flag is related to
 * the inner headers.
 */
#define RTE_MBUF_F_TX_IPV6 (1ULL << 56)

struct mbuf {
	struct mbuf *next; /* the next mbuf in the mbufq */
	unsigned char *head; /* start of the buffer */
	unsigned char *data; /* current position within the buffer */
	unsigned short head_len; /* length of the entire buffer from @head */
	unsigned short len; /* length of the data */
	unsigned short transport_off; /* the offset of the transport header */
	unsigned short network_off; /* the offset of the network header */
	uint32_t tx_dst_ip; /* dest IP (hint for loopback) */
	union {
		struct {
			uint16_t tx_l4_sport; /* used for loopback hash */
			uint16_t tx_l4_dport; /* used for loopback hash */
		};
		uint32_t hash; /* stores computed loopback hash */
	};

	union {
		uint8_t csum_type; /* RX: type of checksum */
		uint8_t txflags; /* TX offload flags */
	};
	uint8_t flags; /* TCP: which flags were set? */
	uint16_t pad;
	atomic_t ref; /* a reference count for the mbuf */
	unsigned long release_data; /* data for the release method */
	void (*release)(struct mbuf *m); /* frees the mbuf */

	/* TCP fields */
	struct list_head link; /* list node for RX and TX queues */
	uint64_t timestamp; /* the time the packet was last sent */
	uint32_t seg_seq; /* the first seg number */
	uint32_t seg_end; /* the last seg number (noninclusive) */
};

static inline unsigned char *__mbuf_pull(struct mbuf *m, unsigned int len)
{
	unsigned char *tmp = m->data;
	m->len -= len;
	m->data += len;
	return tmp;
}

/**
 * mbuf_pull - strips data from the beginning of the buffer
 * @m: the packet
 * @len: the length in bytes to strip
 *
 * Returns the previous start of the buffer. 
 */
static inline unsigned char *mbuf_pull(struct mbuf *m, unsigned int len)
{
	return __mbuf_pull(m, len);
}

/**
 * mbuf_pull_or_null - strips data from the beginning of the buffer
 * @m: the packet
 * @len: the length in bytes to strip
 *
 * Returns the previous start of the buffer or NULL if the buffer is smaller
 * than @len.
 */
static inline unsigned char *mbuf_pull_or_null(struct mbuf *m, unsigned int len)
{
	return m->len >= len ? __mbuf_pull(m, len) : NULL;
}

/**
 * mbuf_push - prepends data to the beginning of the buffer
 * @m: the packet
 * @len: the length in bytes to prepend
 *
 * Returns the new start of the buffer. 
 */
static inline unsigned char *mbuf_push(struct mbuf *m, unsigned int len)
{
	m->data -= len;
	m->len += len;
	return m->data;
}

/**
 * mbuf_put - appends data to the end of the buffer
 * @m: the packet
 * @len: the length in bytes to append
 *
 * Returns the previous end of the buffer. 
 */
static inline unsigned char *mbuf_put(struct mbuf *m, unsigned int len)
{
	unsigned char *tmp = m->data + m->len;
	m->len += len;
	return tmp;
}

/**
 * mbuf_trim - strips data off the end of the buffer
 * @m: the packet
 * @len: the length in bytes to strip
 *
 * Returns a pointer to the start of the bytes that were stripped.
 */
static inline unsigned char *mbuf_trim(struct mbuf *m, unsigned int len)
{
	m->len -= len;
	return m->data + m->len;
}

/**
 * mbuf_reset - forces an mbuf to reposition the data pointer to an offset
 * @m: the packet
 * @offset: the new data offset from @m->head
 */
static inline void mbuf_reset(struct mbuf *m, unsigned int offset)
{
	unsigned int total_len = m->data - m->head + m->len;
	m->data = m->head + offset;
	m->len = total_len - offset;
}

/**
 * mbuf_headroom - returns the space available before the start of the buffer
 * @m: the packet
 */
static inline unsigned int mbuf_headroom(struct mbuf *m)
{
	return m->data - m->head;
}

/**
 * mbuf_tailroom - returns the space available after the end of the buffer
 * @m: the packet
 */
static inline unsigned int mbuf_tailroom(struct mbuf *m)
{
	return m->head + m->head_len - m->data - m->len;
}

/**
 * mbuf_data - returns the current data pointer
 * @m: the packet
 */
static inline unsigned char *mbuf_data(struct mbuf *m)
{
	return m->data;
}

/**
 * mbuf_length - returns the current data length
 * @m: the packet
 */
static inline unsigned int mbuf_length(struct mbuf *m)
{
	return m->len;
}

/*
 * These marcos automatically typecast and determine the size of header structs.
 * In most situations you should use these instead of the raw ops above.
 */
#define mbuf_pull_hdr(mbuf, hdr) (typeof(hdr) *)mbuf_pull(mbuf, sizeof(hdr))

#define mbuf_pull_hdr_or_null(mbuf, hdr) \
	(typeof(hdr) *)mbuf_pull_or_null(mbuf, sizeof(hdr))

#define mbuf_push_hdr(mbuf, hdr) (typeof(hdr) *)mbuf_push(mbuf, sizeof(hdr))

#define mbuf_put_hdr(mbuf, hdr) (typeof(hdr) *)mbuf_put(mbuf, sizeof(hdr))

#define mbuf_trim_hdr(mbuf, hdr) (typeof(hdr) *)mbuf_trim(mbuf, sizeof(hdr))

/**
 * mbuf_mark_network_offset - sets the network offset to the data pointer
 * @m: the mbuf in which to set the network offset
 */
static inline void mbuf_mark_network_offset(struct mbuf *m)
{
	ptrdiff_t off = m->data - m->head;
	m->network_off = off;
}

/**
 * mbuf_mark_transport_offset - sets the transport offset to the data pointer
 * @m: the mbuf in which to set the transport offset
 */
static inline void mbuf_mark_transport_offset(struct mbuf *m)
{
	ptrdiff_t off = m->data - m->head;
	m->transport_off = off;
}

/**
 * mbuf_network_offset - returns a pointer to the network header
 * @m: the mbuf containing the network offset
 */
static inline unsigned char *mbuf_network_offset(struct mbuf *m)
{
	return m->head + m->network_off;
}

/**
 * mbuf_network_offset - returns a pointer to the transport header
 * @m: the mbuf containing the transport offset
 */
static inline unsigned char *mbuf_transport_offset(struct mbuf *m)
{
	return m->head + m->transport_off;
}

#define mbuf_network_hdr(mbuf, hdr) (typeof(hdr) *)mbuf_network_offset(mbuf)

#define mbuf_transport_hdr(mbuf, hdr) (typeof(hdr) *)mbuf_transport_offset(mbuf)

static inline void mbuf_mark_l4_ports(struct mbuf *m, uint16_t sport,
				      uint16_t dport)
{
	m->tx_l4_sport = sport;
	m->tx_l4_dport = dport;
}

static inline void mbuf_mark_dst_ip(struct mbuf *m, uint32_t daddr)
{
	m->tx_dst_ip = daddr;
}

/**
 * mbuf_init - initializes an mbuf
 * @m: the packet to initialize
 * @head: the start of the backing buffer
 * @head_len: the length of backing buffer
 * @reserve_len: the number of bytes to reserve at the start of @head
 */
static inline void mbuf_init(struct mbuf *m, unsigned char *head,
			     unsigned int head_len, unsigned int reserve_len)
{
	m->head = head;
	m->head_len = head_len;
	m->data = m->head + reserve_len;
	m->len = 0;
	INIT_LIST_HEAD(&m->link);
}

/**
 * mbuf_free - frees an mbuf back to an allocator
 * @m: the mbuf to free
 */
static inline void mbuf_free(struct mbuf *m)
{
	m->release(m);
}

/**
 * mbuf_drop - frees an mbuf, counting it as a drop
 * @m: the mbuf to free
 */
static inline void mbuf_drop(struct mbuf *m)
{
	mbuf_free(m);
}

struct mbufq {
	struct mbuf *head, *tail;
};

/**
 * mbufq_push_tail - push an mbuf to the tail of the queue
 * @q: the mbuf queue
 * @m: the mbuf to push
 */
static inline void mbufq_push_tail(struct mbufq *q, struct mbuf *m)
{
	m->next = NULL;
	if (!q->head) {
		q->head = q->tail = m;
		return;
	}
	q->tail->next = m;
	q->tail = m;
}

/**
 * mbufq_pop_head - pop an mbuf from the head of the queue
 * @q: the mbuf queue
 *
 * Returns an mbuf or NULL if the queue is empty.
 */
static inline struct mbuf *mbufq_pop_head(struct mbufq *q)
{
	struct mbuf *head = q->head;
	if (!head)
		return NULL;
	q->head = head->next;
	return head;
}

/**
 * mbufq_peak_head - reads the head of the queue without popping
 * @q: the mbuf queue
 *
 * Returns an mbuf or NULL if the queue is empty.
 */
static inline struct mbuf *mbufq_peak_head(struct mbufq *q)
{
	return q->head;
}

/**
 * mbufq_merge_to_tail - merges a queue to the end of another queue
 * @dst: the destination queue (will contain all the mbufs)
 * @src: the source queue (will become empty)
 */
static inline void mbufq_merge_to_tail(struct mbufq *dst, struct mbufq *src)
{
	if (!src->head)
		return;
	if (!dst->head)
		dst->head = src->head;
	else
		dst->tail->next = src->head;
	dst->tail = src->tail;
	src->head = NULL;
}

/**
 * mbufq_empty - returns true if the queue is empty
 */
static inline bool mbufq_empty(struct mbufq *q)
{
	return q->head == NULL;
}

/**
 * mbufq_release - frees all the mbufs in the queue
 * @q: the queue to release
 */
static inline void mbufq_release(struct mbufq *q)
{
	struct mbuf *m;
	while (true) {
		m = mbufq_pop_head(q);
		if (!m)
			break;
		mbuf_free(m);
	}
}

/**
 * mbufq_init - initializes a queue
 * @q: the mbuf queue to initialize
 */
static inline void mbufq_init(struct mbufq *q)
{
	q->head = NULL;
}

/**
 * chksum_internet - performs an internet checksum on a buffer
 * @buf: the buffer
 * @len: the length in bytes
 *
 * An internet checksum is a 16-bit one's complement sum. Details
 * are described in RFC 1071.
 *
 * Returns a 16-bit checksum value.
 */
static inline uint16_t chksum_internet(const void *buf, int len)
{
	uint64_t sum;

	asm volatile("xorq %0, %0\n"

		     /* process 8 byte chunks */
		     "movl %2, %%edx\n"
		     "shrl $3, %%edx\n"
		     "cmp $0, %%edx\n"
		     "jz 2f\n"
		     "1: adcq (%1), %0\n"
		     "leaq 8(%1), %1\n"
		     "decl %%edx\n"
		     "jne 1b\n"
		     "adcq $0, %0\n"

		     /* process 4 byte (if left) */
		     "2: test $4, %2\n"
		     "je 3f\n"
		     "movl (%1), %%edx\n"
		     "addq %%rdx, %0\n"
		     "adcq $0, %0\n"
		     "leaq 4(%1), %1\n"

		     /* process 2 byte (if left) */
		     "3: test $2, %2\n"
		     "je 4f\n"
		     "movzwq (%1), %%rdx\n"
		     "addq %%rdx, %0\n"
		     "adcq $0, %0\n"
		     "leaq 2(%1), %1\n"

		     /* process 1 byte (if left) */
		     "4: test $1, %2\n"
		     "je 5f\n"
		     "movzbq (%1), %%rdx\n"
		     "addq %%rdx, %0\n"
		     "adcq $0, %0\n"

		     /* fold into 16-bit answer */
		     "5: movq %0, %1\n"
		     "shrq $32, %0\n"
		     "addl %k1, %k0\n"
		     "adcl $0, %k0\n"
		     "movq %0, %1\n"
		     "shrl $16, %k0\n"
		     "addw %w1, %w0\n"
		     "adcw $0, %w0\n"
		     "not %0\n"

		     : "=&r"(sum), "=r"(buf)
		     : "r"(len), "1"(buf)
		     : "%rdx", "cc", "memory");

	return (uint16_t)sum;
}

#define MBUF_HEAD_LEN (ALIGN(sizeof(struct mbuf), L1_CACHE_BYTES))

/**
 * Mask of bits used to determine the status of RX IP checksum.
 * - RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN: no information about the RX IP checksum
 * - RTE_MBUF_F_RX_IP_CKSUM_BAD: the IP checksum in the packet is wrong
 * - RTE_MBUF_F_RX_IP_CKSUM_GOOD: the IP checksum in the packet is valid
 * - RTE_MBUF_F_RX_IP_CKSUM_NONE: the IP checksum is not correct in the packet
 *   data, but the integrity of the IP header is verified.
 */
#define RTE_MBUF_F_RX_IP_CKSUM_MASK ((1ULL << 4) | (1ULL << 7))

#define RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_IP_CKSUM_BAD (1ULL << 4)
#define RTE_MBUF_F_RX_IP_CKSUM_GOOD (1ULL << 7)
#define RTE_MBUF_F_RX_IP_CKSUM_NONE ((1ULL << 4) | (1ULL << 7))

#endif /* _FLUX_IOK_NET_MBUF_H */
