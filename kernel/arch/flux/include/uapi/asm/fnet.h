#ifndef _ASM_UAPI_FLUX_FNET_H
#define _ASM_UAPI_FLUX_FNET_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

#define FLUX_FNET_MBUF_PRIV_SIZE 0

#define FLUX_FNET_IOCTL_ADD 0x4C80

enum flux_fnet_mode {
	FLUX_FNET_MODE_KERNEL = 1,
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
