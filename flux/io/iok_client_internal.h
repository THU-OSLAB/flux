#ifndef _FLUX_LIB_IO_IOK_CLIENT_INTERNAL_H
#define _FLUX_LIB_IO_IOK_CLIENT_INTERNAL_H

#include "iok_client.h"

enum flux_iok_backend_mode {
	FLUX_IOK_BACKEND_NONE,
	FLUX_IOK_BACKEND_LRPC,
	FLUX_IOK_BACKEND_MLX5,
};

struct flux_iok_client_common_state {
	int fd;
	int client_id;
	bool ready;
	bool net_enabled;
	uintptr_t window_base;
	uintptr_t dma_next_addr;
	void *shm_base;
	size_t shm_len;
	void *timer_base;
	size_t timer_len;
	unsigned int mtu;
	int csum_offload;
	unsigned char host_mac[6];
};

struct flux_iok_client_lrpc_state {
	void *rx_base;
	size_t rx_len;
	void *base;
	size_t len;
	struct flux_iok_client_pcpu *pcpus;
};

struct flux_iok_client_mlx5_state {
	bool prepared;
	bool dma_done;
	bool active;
	void *base;
	size_t len;
	void *bar;
	size_t bar_len;
	struct flux_fnet_mlx5_spec *spec;
	struct flux_fnet_mlx5_shared_state *state;
};

struct flux_iok_client_state {
	struct flux_iok_client_common_state common;
	enum flux_iok_backend_mode mode;
	union {
		struct flux_iok_client_lrpc_state lrpc;
		struct flux_iok_client_mlx5_state mlx5;
	} backend;
};

extern struct flux_iok_client_state flux_iok_client;

int flux_iok_socket_send(int fd, const void *buf, size_t len, int send_fd);
int flux_iok_socket_recv(int fd, void *buf, size_t len, int *recv_fd);
int flux_iok_socket_recv_fds(int fd, void *buf, size_t len, int *recv_fds,
			     size_t nr_fds);

size_t flux_iok_client_kernel_dma_size(void);

#ifdef CONFIG_FLUX_FNET
int flux_iok_mlx5_receive_prepare(void);
int flux_iok_mlx5_finish_dma_map(void);
int flux_iok_mlx5_activate(void);
void flux_iok_mlx5_quiesce(void);
void flux_iok_mlx5_cleanup(void);
void flux_iok_mlx5_fill_netdev(struct flux_fnet_netdev *dev);
#else
static inline int flux_iok_mlx5_receive_prepare(void)
{
	return -FLUX_ENODEV;
}

static inline int flux_iok_mlx5_finish_dma_map(void)
{
	return -FLUX_ENODEV;
}

static inline int flux_iok_mlx5_activate(void)
{
	return -FLUX_ENODEV;
}

static inline void flux_iok_mlx5_quiesce(void)
{
}
static inline void flux_iok_mlx5_cleanup(void)
{
}
static inline void flux_iok_mlx5_fill_netdev(struct flux_fnet_netdev *dev)
{
	(void)dev;
}
#endif

#endif /* _FLUX_LIB_IO_IOK_CLIENT_INTERNAL_H */
