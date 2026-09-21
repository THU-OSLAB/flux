#ifndef _FLUX_FNET_MLX5_H
#define _FLUX_FNET_MLX5_H

#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <uapi/asm/fnet.h>

#include "mbuf.h"

#define FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE 18U
#define FNET_MLX5_CQE_SIZE 64U
#define FNET_MLX5_SQ_CLEAN_THRESH 32U
#define FNET_MLX5_SQ_CLEAN_MAX FNET_MLX5_SQ_CLEAN_THRESH
#define FNET_MLX5_RX_BATCH_SIZE 32U

struct fnet_cpu;
struct fnet_netdev;
struct fnet_mlx5;
struct fnet_mlx5_rxq;

/* mlx5 PRM layouts used directly by the data path. */
struct fnet_mlx5_cqe64 {
	union {
		struct {
			u8 rsvd0[2];
			__be16 wqe_id;
			u8 rsvd4[13];
			u8 ml_path;
			u8 rsvd20[4];
			__be16 slid;
			__be32 flags_rqpn;
			u8 hds_ip_ext;
			u8 l4_hdr_type_etc;
			__be16 vlan_info;
		};
		u8 rsvd_first[32];
	};
	__be32 srqn_uidx;
	__be32 imm_inval_pkey;
	u8 app;
	u8 app_op;
	__be16 app_info;
	__be32 byte_cnt;
	__be64 timestamp;
	__be32 sop_drop_qpn;
	__be16 wqe_counter;
	u8 signature;
	u8 op_own;
} __packed;

struct fnet_mlx5_err_cqe {
	u8 rsvd0[32];
	__be32 srqn;
	u8 rsvd1[18];
	u8 vendor_err_synd;
	u8 syndrome;
	__be32 s_wqe_opcode_qpn;
	__be16 wqe_counter;
	u8 signature;
	u8 op_own;
} __packed;

struct fnet_mlx5_wqe_data_seg {
	__be32 byte_count;
	__be32 lkey;
	__be64 addr;
} __packed;

struct fnet_mlx5_wqe_ctrl_seg {
	__be32 opmod_idx_opcode;
	__be32 qpn_ds;
	u8 signature;
	__be16 dci_stream_channel_id;
	u8 fm_ce_se;
	__be32 imm;
} __packed __aligned(4);

struct fnet_mlx5_wqe_eth_seg {
	__be32 rsvd0;
	u8 cs_flags;
	u8 rsvd1;
	__be16 mss;
	__be32 rsvd2;
	__be16 inline_hdr_sz;
	u8 inline_hdr_start[2];
	u8 inline_hdr[16];
} __packed;

struct fnet_mlx5_cq {
	struct fnet_mlx5_cqe64 *cqes;
	u32 *dbr;
	u32 cnt;
	u32 head;
};

struct fnet_mlx5_wq {
	void *buf;
	void **buffers;
	u32 *dbr;
	u32 head;
	u32 cnt;
	u32 log_stride;
};

struct fnet_mlx5_rx_meta {
	struct fnet_mlx5_rx_meta *next;
	struct mbuf mbuf;
	struct fnet_mlx5 *mlx5;
	void *buf;
	u32 cookie;
};

struct fnet_mlx5_rx_cache {
	struct fnet_mlx5_rx_meta *head;
	u32 count;
};

struct fnet_mlx5_rxq {
	struct fnet_mlx5_cq cq;
	struct fnet_mlx5_wq wq;
	struct fnet_mlx5 *mlx5;
	u32 owner_cpu;
	bool stopped;
} ____cacheline_aligned;

struct fnet_mlx5_txq {
	struct fnet_mlx5_wq wq;
	struct fnet_mlx5 *mlx5;
	u32 bf_offset;
	u32 bf_size;
	void *bf_reg;
	struct fnet_mlx5_cq cq;
	u32 owner_cpu;
	bool stopped;
} ____cacheline_aligned;

struct fnet_mlx5 {
	struct flux_fnet_mlx5_spec *spec;
	struct flux_fnet_mlx5_shared_state *state;
	struct fnet_mlx5_rxq *rxqs;
	struct fnet_mlx5_txq *txqs;
	struct fnet_mlx5_rx_cache __percpu *rx_caches;
	spinlock_t rx_pool_lock;
	struct fnet_mlx5_rx_meta *rx_pool;
	u32 rx_pool_count;
	u32 rx_pool_total;
	u32 rx_upper_owned;
	void *mem_base;
	u64 mem_len;
	void *bar_base;
	u64 bar_len;
	u32 generation;
	u32 nr_queues;
	bool active;
	bool quiescing;
};

int fnet_mlx5_init(struct fnet_netdev *fnet,
		   const struct flux_fnet_netdev *arg);
void fnet_mlx5_deactivate(struct fnet_netdev *fnet);
void fnet_mlx5_quiesce(struct fnet_netdev *fnet);
int fnet_mlx5_destroy(struct fnet_netdev *fnet);
int fnet_mlx5_poll(struct fnet_cpu *cpu, int budget);
bool fnet_mlx5_tx(struct fnet_cpu *cpu, struct mbuf *m);
bool fnet_mlx5_xmit_skb(struct fnet_cpu *cpu, struct sk_buff *skb);

extern const struct fnet_backend_ops fnet_mlx5_ops;

#endif
