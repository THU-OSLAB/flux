#ifndef _ASM_UAPI_FLUX_FNET_H
#define _ASM_UAPI_FLUX_FNET_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u16 flux_fnet_u16;
typedef __u32 flux_fnet_u32;
typedef __u64 flux_fnet_u64;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint16_t flux_fnet_u16;
typedef uint32_t flux_fnet_u32;
typedef uint64_t flux_fnet_u64;
#endif

#define FLUX_FNET_MTU 1500
#define FLUX_FNET_PORT 0
#define FLUX_FNET_RX_BURST_SIZE 64
#define FLUX_FNET_TX_BURST_SIZE 64
#define FLUX_FNET_RX_RING_SIZE 256
#define FLUX_FNET_TX_RING_SIZE 256

#define FLUX_FNET_MLX5_RX_RING_SIZE 2048
#define FLUX_FNET_MLX5_TX_RING_SIZE 2048

#define FLUX_FNET_RX_POOL_SIZE (8192 * 16)
#define FLUX_FNET_TX_POOL_SIZE (8192 * 16)
#define FLUX_FNET_RX_POOL_CACHE_SIZE 256
#define FLUX_FNET_TX_POOL_CACHE_SIZE 256
#define FLUX_FNET_SHBUF_SIZE 0x80000000
#define FLUX_FNET_LRPC_QUEUE_SIZE 4096

#define FLUX_FNET_MLX5_SPEC_MAGIC 0x46354d58U
#define FLUX_FNET_MLX5_SPEC_VERSION 1U
#define FLUX_FNET_MLX5_MAX_QUEUES 64U

#define FLUX_FNET_MLX5_CAP_REGULAR_RQ (1U << 0)
#define FLUX_FNET_MLX5_CAP_CQE64 (1U << 1)
#define FLUX_FNET_MLX5_CAP_FIXED_RQT (1U << 2)
#define FLUX_FNET_MLX5_CAP_DIRECT_HCA_VA (1U << 3)
#define FLUX_FNET_MLX5_CAP_VFIO_BAR (1U << 4)

enum flux_fnet_mlx5_state {
	FLUX_FNET_MLX5_STATE_ATTACHED = 0,
	FLUX_FNET_MLX5_STATE_PREPARED,
	FLUX_FNET_MLX5_STATE_TX_MR_READY,
	FLUX_FNET_MLX5_STATE_FNET_READY,
	FLUX_FNET_MLX5_STATE_ACTIVE,
	FLUX_FNET_MLX5_STATE_QUIESCING,
	FLUX_FNET_MLX5_STATE_DEAD,
};

struct flux_fnet_mlx5_ring_spec {
	flux_fnet_u64 buf_offset;
	flux_fnet_u64 dbr_offset;
	flux_fnet_u32 nr_entries;
	flux_fnet_u32 stride;
};

struct flux_fnet_mlx5_queue_spec {
	flux_fnet_u32 owner_cpu;
	flux_fnet_u32 cqn_rx;
	flux_fnet_u32 cqn_tx;
	flux_fnet_u32 rqn;
	flux_fnet_u32 sqn;
	flux_fnet_u32 uarn;
	flux_fnet_u32 reserved;
	flux_fnet_u64 bar_page_offset;
	flux_fnet_u32 bf_offset;
	flux_fnet_u32 bf_size;
	struct flux_fnet_mlx5_ring_spec rx_cq;
	struct flux_fnet_mlx5_ring_spec rx_wq;
	struct flux_fnet_mlx5_ring_spec tx_cq;
	struct flux_fnet_mlx5_ring_spec tx_wq;
};

struct flux_fnet_mlx5_spec {
	flux_fnet_u32 magic;
	flux_fnet_u16 version;
	flux_fnet_u16 header_len;
	flux_fnet_u32 total_len;
	flux_fnet_u32 capabilities;
	flux_fnet_u32 generation;
	flux_fnet_u32 nr_queues;
	flux_fnet_u32 mtu;
	flux_fnet_u64 memfd_len;
	flux_fnet_u64 state_offset;
	flux_fnet_u64 rx_buf_offset;
	flux_fnet_u64 rx_buf_len;
	flux_fnet_u32 rx_buf_stride;
	flux_fnet_u32 rx_headroom;
	flux_fnet_u32 rx_buf_count;
	flux_fnet_u32 reserved_rx;
	flux_fnet_u64 rx_hca_va;
	flux_fnet_u64 rx_mr_len;
	flux_fnet_u32 rx_lkey;
	flux_fnet_u32 reserved0;
	flux_fnet_u64 tx_hca_va;
	flux_fnet_u64 tx_mr_len;
	flux_fnet_u32 tx_lkey;
	flux_fnet_u32 reserved1;
	flux_fnet_u64 bar_offset;
	flux_fnet_u64 bar_len;
	struct flux_fnet_mlx5_queue_spec queues[];
};

struct flux_fnet_mlx5_shared_state {
	flux_fnet_u32 state;
	flux_fnet_u32 generation;
	flux_fnet_u32 last_error;
	flux_fnet_u32 last_syndrome;
	flux_fnet_u64 ready_queues;
	flux_fnet_u64 outstanding_rx;
	flux_fnet_u64 inflight_tx;
};

#define FLUX_FNET_MBUF_PRIV_SIZE 0

#define FLUX_FNET_IOCTL_ADD 0x4C80
#define FLUX_FNET_IOCTL_DEL 0x4C81

enum flux_fnet_mode {
	FLUX_FNET_MODE_KERNEL = 1,
	FLUX_FNET_MODE_MLX5_EXTERNAL = 2,
};

struct flux_fnet_qspec {
	void *tbl;
	uint32_t size;
	uint32_t *wb;
};

struct flux_fnet_netdev {
	struct flux_fnet_netdev *next;
	unsigned int port_id;
	unsigned int nb_rx_queues;
	unsigned int nb_tx_queues;
	unsigned int mtu;
	enum flux_fnet_mode mode;
	int gso_offload;
	int csum_offload;
	void *tx_mempool;
	uint8_t eth_addr[6];
	uint32_t addr;
	uint32_t netmask;
	uint32_t gateway;
	void *rx_base;
	uint32_t rx_len;
	void *mlx5_base;
	uint64_t mlx5_len;
	void *mlx5_bar;
	uint64_t mlx5_bar_len;
	struct flux_fnet_mlx5_spec *mlx5_spec;

	struct flux_fnet_qspec *rxqs;
	struct flux_fnet_qspec *txpktqs;
	struct flux_fnet_qspec *txcmdqs;
};

enum {
	/*
	 * Hardware did not provide checksum information.
	 */
	FLUX_CHKSUM_TYPE_NEEDED = 0,

	/*
	 * The checksum was verified by hardware and found to be valid.
	 */
	FLUX_CHKSUM_TYPE_UNNECESSARY,

	/*
	 * Hardware provided a 16 bit one's complement sum from after the LL
	 * header to the end of the packet. VLAN tags (if present) are included
	 * in the sum. This is the most robust checksum type because it's useful
	 * even if the NIC can't parse the headers.
	 */
	FLUX_CHKSUM_TYPE_COMPLETE,

	FLUX_CHKSUM_TYPE_NR,
};

enum {
	FLUX_RX_NET_RECV = 0,
	FLUX_RX_NET_COMP,
};

union flux_rxq_cmd {
	struct {
		uint16_t rxcmd;
		uint16_t csum_type;
		uint32_t len;
	};
	uint64_t lrpc_cmd;
};

#define FLUX_RXQ_PAYLOAD_FAST_NET_MASK (1UL << 63)
#define FLUX_RXQ_PAYLOAD_DATA_MASK (~FLUX_RXQ_PAYLOAD_FAST_NET_MASK)

static inline bool flux_rxq_payload_is_fast_net(uint64_t payload)
{
	return (payload & FLUX_RXQ_PAYLOAD_FAST_NET_MASK) != 0;
}

static inline uint64_t flux_rxq_payload_to_rx_offset(uint64_t payload)
{
	return payload & FLUX_RXQ_PAYLOAD_DATA_MASK;
}

static inline uint64_t flux_rxq_payload_from_rx_offset(uint64_t offset)
{
	return offset & FLUX_RXQ_PAYLOAD_DATA_MASK;
}

static inline uint64_t flux_rxq_payload_from_fast_net_rx_offset(uint64_t offset)
{
	return flux_rxq_payload_from_rx_offset(offset) |
	       FLUX_RXQ_PAYLOAD_FAST_NET_MASK;
}

static inline void *flux_txpkt_mbuf_data_from_ptr(uint64_t ptr)
{
	/* offset of mbuf.data */
	return *(void **)(ptr + 16);
}

enum {
	FLUX_TXPKT_NET_XMIT = 0,
	FLUX_TXPKT_NR, /* number of commands */
};

#define FLUX_OLFLAG_IP_CHKSUM (1 << 0)
#define FLUX_OLFLAG_L3_CHKSUM (1 << 1)
#define FLUX_OLFLAG_IPV4 (1 << 2)
#define FLUX_OLFLAG_IPV6 (1 << 3)

union flux_txpktq_cmd {
	struct {
		uint32_t dst_ip;
		uint16_t len;
		uint8_t olflags;
		uint8_t txcmd; // Top bit must be 0.
	};
	uint64_t lrpc_cmd;
};

#define FLUX_TXPKT_PAYLOAD_DATA_MASK ((1ULL << 48) - 1)

static inline uint64_t flux_txpkt_offset_to_payload(uint64_t offset,
						    uint16_t rss)
{
	return (offset & FLUX_TXPKT_PAYLOAD_DATA_MASK) | ((uint64_t)rss << 48);
}

static inline uint64_t flux_txpkt_offset_from_payload(uint64_t payload)
{
	return payload & FLUX_TXPKT_PAYLOAD_DATA_MASK;
}

static inline uint64_t flux_txpkt_payload_from_ptr(uint64_t ptr, uint64_t base,
						   uint16_t rss)
{
	return flux_txpkt_offset_to_payload(ptr - base, rss);
}

static inline uint64_t flux_txpkt_ptr_from_payload(uint64_t payload,
						   uint64_t base)
{
	return base + flux_txpkt_offset_from_payload(payload);
}

static inline uint64_t flux_rss_from_txpkt_payload(uint64_t payload)
{
	return payload >> 48;
}

enum {
	FLUX_TXCMD_NET_COMP = 0,
	FLUX_TXCMD_NR, /* number of commands */
};

union flux_txcmdq_cmd {
	struct {
		uint16_t txcmd;
		uint16_t pad;
		uint32_t rsvd; // Upper bit must be zero.
	};
	uint64_t lrpc_cmd;
};

#endif /* _ASM_UAPI_FLUX_FNET_H */
