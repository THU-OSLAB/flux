#ifndef _FLUX_IOKD_H
#define _FLUX_IOKD_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <utils/base.h>
#include <utils/lrpc.h>
#include <utils/time.h>

#include "host/kmod.h"
#include "io/iok_ctrl.h"
#include "io/iok_ext.h"

struct rte_mempool;
struct rte_mbuf;
struct rte_hash;

#define FLUX_IOKD_MAX_CLIENTS 8
#define FLUX_IOKD_RX_PREFETCH_STRIDE 2
#define FLUX_IOKD_CTRL_BACKLOG 64

struct flux_iokd_config {
	char *config_path;
	char *sock_path;
	char *nic_pci_addr;
	char *cpu_range;
	int cpu;
	unsigned int mtu;
	bool tx_copy;
	bool tx_chksum_offload;
	bool no_network;
	bool mlx5_external;
};

struct flux_iokd_dma_map {
	uintptr_t client_addr;
	uintptr_t iokd_addr;
	size_t len;
	size_t pgsize;
	struct flux_iokd_dma_map *next;
};

struct flux_iokd_pcpu {
	struct lrpc_chan_out rxq;
	struct lrpc_chan_in txpktq;
	struct lrpc_chan_in txcmdq;
};

struct flux_iokd_client {
	pthread_t control_thread;
	bool control_thread_started;
	bool mlx5_clean_shutdown;
	int id;
	int fd;
	bool ready;
	bool retired;
	bool retire_requested;
	unsigned int refcnt;
	pid_t peer_pid;
	uid_t peer_uid;
	bool cpu_assignment_active;
	uint32_t ip_addr;
	uint32_t netmask;
	uint32_t gateway;
	uint32_t io_weight;
	uint32_t net_class_id;
	uint32_t net_priority;
	uint32_t cpu_shares;
	int64_t cpu_quota;
	uint64_t cpu_period;
	uint64_t tx_deficit;
	uint64_t tx_quota_window_start_ns;
	uint64_t tx_quota_used_ns;
	int nr_cpus;
	int cpu_list[CONFIG_FLUX_MAX_CPUS];
	uintptr_t window_base;
	void *shm_base;
	size_t shm_len;
	void *timer_base;
	size_t timer_len;
	void *rx_base;
	size_t rx_len;
	struct flux_iokd_pcpu pcpus[CONFIG_FLUX_MAX_CPUS];
	struct rte_mbuf *owned_rx_bufs;
	struct flux_iokd_dma_map *dma_maps;
	void *mlx5;
	size_t tx_comp_nr_ofs;
	size_t tx_comp_max_ofs;
	unsigned long *tx_comp_ofq;
	unsigned int tx_next_cpu_rr;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t tx_packets;
	uint64_t tx_bytes;
	struct flux_iokd_client *pending_reg_next;
	struct flux_iokd_client *pending_unreg_next;
	struct flux_iokd_client *retired_next;
};

struct flux_iokd_tx_priv {
	struct flux_iokd_client *client;
	int cpu_idx;
	unsigned long comp_data;
};

struct flux_iokd_pkt {
	union flux_txpktq_cmd cmd;
	unsigned long comp_data;
	char *buf;
	struct flux_iokd_client *client;
	int cpu_idx;
	uint32_t dst_ip;
	uint16_t len;
	uint16_t hash;
	uint8_t olflags;
};

uint32_t flux_iokd_client_tx_quantum(const struct flux_iokd_client *client);

struct flux_iokd {
	int listen_fd;
	pthread_mutex_t cpu_lock;
	pthread_mutex_t control_lock;
	pthread_cond_t control_cond;
	pthread_t accept_thread;
	pthread_t control_worker_thread;
	struct flux_iok_ctrl ctrl;
	bool *cpu_busy;
	bool *cpu_allowed;
	int nr_host_cpus;
	int nr_allowed_cpus;
	struct rte_mempool *rx_mempool;
	struct rte_mempool *tx_mempool;
	struct rte_hash *client_ip_hash;
	struct flux_iokd_client *pending_clients;
	struct flux_iokd_client *pending_unregister_clients;
	struct flux_iokd_client *clients[FLUX_IOKD_MAX_CLIENTS];
	struct flux_iokd_client *retired_clients;
	pid_t client_pids[FLUX_IOKD_MAX_CLIENTS];
	unsigned int next_client_id;
	int control_cpu;
	bool accept_thread_started;
	bool control_worker_started;
	bool control_work_pending;
	bool control_worker_stop;
	bool fnet_has_port;
	bool fnet_is_tap;
	bool tx_chksum_offload;
	bool tx_comp_pending;
	void *mlx5_device;
};

extern struct flux_iokd_config flux_iokd_cfg;
extern struct flux_iokd flux_iokd;
extern struct flux_shm *flux_shm;

static inline struct flux_iokd_client *flux_iokd_client_load(int idx)
{
	return atomic_load_acquire(&flux_iokd.clients[idx]);
}

static inline void flux_iokd_client_store(int idx,
					  struct flux_iokd_client *client)
{
	atomic_store_release(&flux_iokd.clients[idx], client);
}

static inline bool flux_iokd_client_ready(const struct flux_iokd_client *client)
{
	return atomic_load_acquire(&client->ready);
}

static inline void flux_iokd_client_set_ready(struct flux_iokd_client *client,
					      bool ready)
{
	atomic_store_release(&client->ready, ready);
}

int flux_iokd_config_load(int argc, char **argv);
void flux_iokd_config_cleanup(void);

int flux_iokd_kmod_map_shm(void);
int flux_iokd_kmod_init_sender(void);
void flux_iokd_kmod_fini_sender(void);

int flux_iokd_fnet_init(void);
void flux_iokd_fnet_stop(void);
void flux_iokd_fnet_fini(void);

#ifdef CONFIG_FLUX_FNET
int flux_iokd_mlx5_init(void);
void flux_iokd_mlx5_fini(void);
int flux_iokd_mlx5_client_prepare(struct flux_iokd_client *client,
				  struct flux_iok_ctrl_mlx5_prepare *reply,
				  int fds[2]);
int flux_iokd_mlx5_dma_map_done(
	struct flux_iokd_client *client,
	const struct flux_iok_ctrl_mlx5_dma_map_done *req);
int flux_iokd_mlx5_client_ready(struct flux_iokd_client *client,
				const struct flux_iok_ctrl_mlx5_ready *req);
int flux_iokd_mlx5_client_quiesce(struct flux_iokd_client *client);
void flux_iokd_mlx5_client_destroy(struct flux_iokd_client *client);
bool flux_iokd_mlx5_dma_range_busy(const struct flux_iokd_client *client,
				   uintptr_t addr, size_t len);
#else
static inline int
flux_iokd_mlx5_client_prepare(struct flux_iokd_client *client,
			      struct flux_iok_ctrl_mlx5_prepare *reply,
			      int fds[2])
{
	(void)client;
	(void)reply;
	(void)fds;
	return -1;
}

static inline int
flux_iokd_mlx5_dma_map_done(struct flux_iokd_client *client,
			    const struct flux_iok_ctrl_mlx5_dma_map_done *req)
{
	(void)client;
	(void)req;
	return -1;
}

static inline int
flux_iokd_mlx5_client_ready(struct flux_iokd_client *client,
			    const struct flux_iok_ctrl_mlx5_ready *req)
{
	(void)client;
	(void)req;
	return -1;
}

static inline int flux_iokd_mlx5_client_quiesce(struct flux_iokd_client *client)
{
	(void)client;
	return -1;
}

static inline void
flux_iokd_mlx5_client_destroy(struct flux_iokd_client *client)
{
	(void)client;
}

static inline bool
flux_iokd_mlx5_dma_range_busy(const struct flux_iokd_client *client,
			      uintptr_t addr, size_t len)
{
	(void)client;
	(void)addr;
	(void)len;
	return false;
}
#endif

int flux_iokd_control_init(void);
int flux_iokd_control_start(void);
void flux_iokd_control_fini(void);
void flux_iokd_control_wake(void);
int flux_iokd_cpu_allocator_init(void);
void flux_iokd_cpu_allocator_fini(void);

bool flux_iokd_rx_burst(void);
int flux_iokd_rx_clients_init(void);
void flux_iokd_rx_clients_fini(void);
int flux_iokd_rx_client_register(struct flux_iokd_client *client);
void flux_iokd_rx_client_unregister(struct flux_iokd_client *client);
bool flux_iokd_timers_run(void);
void flux_iokd_reap_retired(void);
void flux_iokd_process_retire_requests(void);
#ifdef CONFIG_FLUX_FNET
void flux_iokd_rx_client_queue_register(struct flux_iokd_client *client);
void flux_iokd_rx_client_queue_unregister(struct flux_iokd_client *client);
void flux_iokd_rx_process_pending_clients(void);
bool flux_iokd_tx_burst(void);
void flux_iokd_tx_shutdown_flush(void);
bool flux_iokd_drain_completions(void);
bool flux_iokd_commands_rx(void);
struct rte_mempool *flux_iokd_tx_pool_create(size_t data_room_size);
bool flux_iokd_rx_mbuf_deliver(struct flux_iokd_client *client, int cpu_idx,
			       struct rte_mbuf *buf);
void flux_iokd_rx_mbuf_complete(struct flux_iokd_client *client,
				unsigned long payload);
void flux_iokd_rx_mbuf_reclaim_all(struct flux_iokd_client *client);
#else
static inline void
flux_iokd_rx_client_queue_register(struct flux_iokd_client *client)
{
	(void)client;
}

static inline void
flux_iokd_rx_client_queue_unregister(struct flux_iokd_client *client)
{
	(void)client;
}

static inline void flux_iokd_rx_process_pending_clients(void)
{
}

static inline bool flux_iokd_tx_burst(void)
{
	return false;
}

static inline void flux_iokd_tx_shutdown_flush(void)
{
}

static inline bool flux_iokd_drain_completions(void)
{
	return false;
}

static inline bool flux_iokd_commands_rx(void)
{
	return false;
}

static inline bool flux_iokd_rx_mbuf_deliver(struct flux_iokd_client *client,
					     int cpu_idx, struct rte_mbuf *buf)
{
	(void)client;
	(void)cpu_idx;
	(void)buf;
	return false;
}

static inline void flux_iokd_rx_mbuf_complete(struct flux_iokd_client *client,
					      unsigned long payload)
{
	(void)client;
	(void)payload;
}

static inline void
flux_iokd_rx_mbuf_reclaim_all(struct flux_iokd_client *client)
{
	(void)client;
}
#endif

void flux_iokd_client_destroy(struct flux_iokd_client *client);
void flux_iokd_client_request_retire(struct flux_iokd_client *client);
void flux_iokd_client_retire(struct flux_iokd_client *client);
bool flux_iokd_client_get(struct flux_iokd_client *client);
void flux_iokd_client_put(struct flux_iokd_client *client);
void flux_iokd_notify_timer(struct flux_iokd_client *client, int cpu_idx);
void flux_iokd_request_client_exit(pid_t pid, int sig);
void flux_iokd_request_all_clients_exit(int sig);
void flux_iokd_retire_all_clients(void);
bool flux_iokd_shutdown_requested(void);
void flux_iokd_request_shutdown(void);
void flux_iokd_kmod_unmap_shm(void);

#endif /* _FLUX_IOKD_H */
