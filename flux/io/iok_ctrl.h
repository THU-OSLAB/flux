#ifndef _FLUX_LIB_IO_IOK_CTRL_H
#define _FLUX_LIB_IO_IOK_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utils/lrpc.h>
#include <utils/memory.h>

struct rte_device;
struct rte_mbuf;
struct rte_mempool;
struct flux_iokd_client;

struct flux_iok_ctrl;

struct flux_iok_ctrl_pcpu {
	struct flux_iok_ctrl *ctrl;
	struct lrpc_chan_out lrpc_rxq;
	struct lrpc_chan_in lrpc_txpktq;
	struct lrpc_chan_in lrpc_txcmdq;
};

struct flux_iok_ctrl {
	struct flux_iok_ctrl *next;
	struct flux_iok_ctrl_pcpu *pcpus;
	uint8_t port;
	bool iova_mode_pa;
	bool dev_init;
	struct rte_device *dev;
	unsigned char rss_key[52];
	size_t rss_key_len;
	uint8_t host_mac[6];

	struct rte_mempool *rx_mempool;
	struct rte_mempool *tx_mempool;
	unsigned int nb_rx_queues;
	unsigned int nb_tx_queues;

	/* rx buffers in shared memory */
	void *rx_shbuf;
	size_t rx_shbuf_len;
	mem_key_t rx_shbuf_key;
	size_t rx_shbuf_pgsize;
	size_t rx_obj_size;
	size_t rx_obj_off;
	bool rx_shbuf_dma_mapped;

	/* lrpc queues */
	void *lrpc_shbuf;
	size_t lrpc_shbuf_len;

	/* overflow queue for completion data */
	size_t tx_comp_nr_ofs;
	size_t tx_comp_max_ofs;
	unsigned long *tx_comp_ofq;
	unsigned int tx_next_cpu_rr;
};

#define FLUX_IOK_TX_COMP_POOL_SIZE 32767
#define FLUX_IOK_TX_DRAIN_COMP_BURST 64

/* io kernel */

struct rx_priv_data {
	struct flux_iokd_client *owner;
	struct rte_mbuf *next_owned;
	struct rte_mbuf *prev_owned;
};

struct tx_priv_data {
	struct flux_iok_ctrl *ctrl;
	struct flux_iok_ctrl_pcpu *pcpu;
	unsigned long comp_data;
};

extern int flux_iok_dma_map(struct flux_iok_ctrl *ctrl, void *buf, size_t len,
			    size_t pgsize, uintptr_t *physaddrs);
extern void flux_iok_dma_unmap(struct flux_iok_ctrl *ctrl, void *buf,
			       size_t len, size_t pgsize, uintptr_t *physaddrs);

#endif /* _FLUX_LIB_IO_IOK_CTRL_H */
