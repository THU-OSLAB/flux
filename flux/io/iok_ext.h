#ifndef _FLUX_LIB_IO_IOK_EXT_H
#define _FLUX_LIB_IO_IOK_EXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/asm/fnet.h>

/*
 * Keep this protocol header independent from <flux.h>/<flux/base.h>.
 *
 * The control-plane and shared-memory layouts are consumed by both the Flux
 * runtime and the external IOK daemon. Pulling in the full Flux syscall/UAPI
 * surface here makes unrelated translation units depend on kernel syscall
 * wrappers and host libc internals.
 */

#define FLUX_IOK_CTRL_MAGIC 0x66696f6bU
#define FLUX_IOK_CTRL_VERSION 4U

#define FLUX_IOKD_NAME_PREFIX "flux_iokd"
#define FLUX_IOK_SOCK_PATH_BASE "/tmp/flux.sock"

#define FLUX_IOK_CLIENT_WINDOW_BASE 0x400000000000ULL
#define FLUX_IOK_CLIENT_WINDOW_SIZE (32ULL << 30)
#define FLUX_IOK_SHARED_OFFSET 0x0ULL
#define FLUX_IOK_SHARED_SIZE (1ULL << 30)
#define FLUX_IOK_DMA_OFFSET (4ULL << 30)
#define FLUX_IOK_DMA_SIZE (24ULL << 30)

#define FLUX_IOK_RX_SLOT_DATA_LEN 2176
#define FLUX_IOK_MBUF_F_RX_IP_CKSUM_MASK ((1ULL << 4) | (1ULL << 7))
#define FLUX_IOK_MBUF_F_RX_IP_CKSUM_GOOD (1ULL << 7)

typedef uint64_t flux_iok_iova_t;

struct flux_iok_mbuf_sched {
	uint32_t queue_id;
	uint8_t traffic_class;
	uint8_t color;
	uint16_t reserved;
};

/*
 * Shared RX slots only need the leading rte_mbuf layout used by the kernel
 * IOK receive path. Keep the field order compatible with the kernel-side
 * definition in kernel/arch/flux/drivers/fnet/mbuf.h.
 */
struct flux_iok_mbuf {
	void *buf_addr;
	flux_iok_iova_t buf_iova;
	uint16_t data_off;
	uint16_t refcnt;
	uint16_t nb_segs;
	uint16_t port;
	uint64_t ol_flags;
	union {
		uint32_t packet_type;
		struct {
			uint8_t l2_type : 4;
			uint8_t l3_type : 4;
			uint8_t l4_type : 4;
			uint8_t tun_type : 4;
			union {
				uint8_t inner_esp_next_proto;
				struct {
					uint8_t inner_l2_type : 4;
					uint8_t inner_l3_type : 4;
				};
			};
			uint8_t inner_l4_type : 4;
		};
	};
	uint32_t pkt_len;
	uint16_t data_len;
	uint16_t vlan_tci;
	union {
		union {
			uint32_t rss;
			struct {
				union {
					struct {
						uint16_t hash;
						uint16_t id;
					};
					uint32_t lo;
				};
				uint32_t hi;
			} fdir;
			struct flux_iok_mbuf_sched sched;
			struct {
				uint32_t reserved1;
				uint16_t reserved2;
				uint16_t txq;
			} txadapter;
			uint32_t usr;
		} hash;
	};
	uint16_t vlan_tci_outer;
	uint16_t buf_len;
};

enum flux_iok_ctrl_op {
	FLUX_IOK_CTRL_ATTACH = 1,
	FLUX_IOK_CTRL_ACK = 2,
	FLUX_IOK_CTRL_REGISTER = 3,
	FLUX_IOK_CTRL_DMA_MAP = 4,
	FLUX_IOK_CTRL_DMA_UNMAP = 5,
};

struct flux_iok_ctrl_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t op;
	uint32_t len;
};

struct flux_iok_ctrl_attach {
	struct flux_iok_ctrl_hdr hdr;
	uint32_t requested_cpus;
	uint32_t ip_addr;
	uint32_t netmask;
	uint32_t gateway;
};

struct flux_iok_ctrl_ack {
	struct flux_iok_ctrl_hdr hdr;
	uint32_t client_id;
	uint32_t mtu;
	uint32_t csum_offload;
	uint32_t nr_cpus;
	uint32_t rx_shm_key;
	uint64_t window_base;
	uint64_t rx_len;
	uint64_t rx_pgsize;
	uint8_t host_mac[6];
	uint8_t reserved[10];
	int cpu_list[CONFIG_FLUX_MAX_CPUS];
};

struct flux_iok_ctrl_register {
	struct flux_iok_ctrl_hdr hdr;
	uint32_t nr_cpus;
	uint32_t pad;
	uint64_t shm_base;
	uint64_t shm_len;
	uint64_t timer_base;
	uint64_t timer_len;
	uint64_t rx_base;
	uint64_t rx_len;
	struct flux_fnet_qspec rxqs[CONFIG_FLUX_MAX_CPUS];
	struct flux_fnet_qspec txpktqs[CONFIG_FLUX_MAX_CPUS];
	struct flux_fnet_qspec txcmdqs[CONFIG_FLUX_MAX_CPUS];
};

struct flux_iok_ctrl_dma_map {
	struct flux_iok_ctrl_hdr hdr;
	uint64_t addr;
	uint64_t len;
	uint64_t pgsize;
};

struct flux_iok_rx_slot {
	struct flux_iok_mbuf mbuf;
	unsigned char data[FLUX_IOK_RX_SLOT_DATA_LEN];
};

struct flux_iok_timer_entry {
	uint64_t deadline_ns;
} __attribute__((__aligned__(64)));

#endif /* _FLUX_LIB_IO_IOK_EXT_H */
