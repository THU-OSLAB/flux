#ifndef _IOK_NET_DEV_H
#define _IOK_NET_DEV_H

#include <linux/netdevice.h>
#include <uapi/asm/fnet.h>

#include "lrpc.h"
#include "mbuf.h"

struct fnet_cpu;
struct fnet_netdev;

struct fnet_mlx5;
struct fnet_mlx5_rxq;
struct fnet_mlx5_txq;

struct fnet_cpu {
	bool init;
	struct fnet_netdev *fnet;
	struct task_struct *thread;
	union {
		struct {
			struct lrpc_chan_in rxq;
			struct lrpc_chan_out txcmdq;
			struct lrpc_chan_out txpktq;
		} lrpc;
		struct {
			struct fnet_mlx5_rxq *rxq;
			struct fnet_mlx5_txq *txq;
		} mlx5;
	};
	/* pending overflow packets */
	struct sk_buff_head txofq_skb;
	struct mbufq txofq_mbuf;
} ____cacheline_aligned;

struct fnet_backend_ops {
	int (*start)(struct fnet_netdev *fnet,
		     const struct flux_fnet_netdev *arg);
	void (*deactivate)(struct fnet_netdev *fnet);
	void (*quiesce)(struct fnet_netdev *fnet);
	int (*stop)(struct fnet_netdev *fnet);
	int (*rx_poll)(struct fnet_cpu *cpu, int budget);
	bool (*xmit_skb)(struct fnet_cpu *cpu, struct sk_buff *skb);
	bool (*xmit_mbuf)(struct fnet_cpu *cpu, struct mbuf *m);
};

DECLARE_PER_CPU_ALIGNED(struct fnet_cpu, fnet_cpus);

static inline struct fnet_cpu *fnet_get_cpu(void)
{
	preempt_disable();
	return this_cpu_ptr(&fnet_cpus);
}

static inline void fnet_put_cpu(void)
{
	preempt_enable();
}

struct fnet_netdev {
	int id;
	struct flux_fnet_netdev arg;
	struct net_device *dev;
	struct fnet_cpu *cpus[NR_CPUS];
	struct fnet_mlx5 *mlx5;
	bool quiesced;
#ifdef CONFIG_FLUX_FAST_NET
	uint8_t fast_net_mac[ETH_ALEN];
	uint32_t fast_net_ip;
	uint32_t fast_net_netmask;
	uint32_t fast_net_gateway;
#endif
};

static inline bool fnet_rx_offset_valid(const struct fnet_netdev *fnet,
					unsigned long offset, unsigned int len)
{
	unsigned long end;

	if (!fnet || !fnet->arg.rx_base)
		return false;
	if (__builtin_add_overflow(offset, len, &end))
		return false;

	return end <= fnet->arg.rx_len;
}

static inline void *fnet_rx_data_from_offset(const struct fnet_netdev *fnet,
					     unsigned long offset,
					     unsigned int len)
{
	if (!fnet_rx_offset_valid(fnet, offset, len))
		return NULL;

	return (unsigned char *)fnet->arg.rx_base + offset;
}

extern struct fnet_netdev *fnet_dev;
extern struct fnet_backend_ops fnet_ops;
extern const struct fnet_backend_ops fnet_lrpc_ops;
extern const struct fnet_backend_ops fnet_mlx5_ops;

#ifdef CONFIG_DEBUG_FNET
#define fnet_dbg(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#else
#define fnet_dbg(fmt, ...) \
	do {               \
	} while (0)
#endif

#endif
