#define _GNU_SOURCE
#define FLUX_FMT "iokd-client-control: "

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <kernel/asm/host_ops.h>
#include <utils/log.h>

#include "internal.h"

static bool flux_iokd_dma_uses_kernel_window(uintptr_t client_addr, size_t len,
					     uintptr_t *offset_out)
{
	uintptr_t offset;
	uintptr_t end;

	if (client_addr < FLUX_MEMORY_ADDR)
		return false;

	offset = client_addr - FLUX_MEMORY_ADDR;
	if (__builtin_add_overflow(offset, len, &end))
		return false;
	if (end > FLUX_IOK_DMA_SIZE)
		return false;

	if (offset_out)
		*offset_out = offset;
	return true;
}

static void *flux_iokd_dma_map_addr(const struct flux_iokd_client *client,
				    uintptr_t client_addr, size_t len)
{
	uintptr_t offset;

	if (!flux_iokd_dma_uses_kernel_window(client_addr, len, &offset))
		return (void *)client_addr;

	return (void *)(client->window_base + FLUX_IOK_DMA_OFFSET + offset);
}

static int flux_iokd_mlx5_message(struct flux_iokd_client *client,
				  const struct flux_iok_ctrl_hdr *hdr,
				  int recv_fd)
{
	switch (hdr->op) {
	case FLUX_IOK_CTRL_MLX5_DMA_MAP_DONE: {
		const struct flux_iok_ctrl_mlx5_dma_map_done *done =
			(const void *)hdr;

		if (recv_fd >= 0 || !flux_iokd_cfg.mlx5_external ||
		    hdr->len != sizeof(*done) ||
		    flux_iokd_mlx5_dma_map_done(client, done) < 0)
			return -1;
		return flux_iokd_socket_send(client->fd, done, sizeof(*done),
					     -1);
	}
	case FLUX_IOK_CTRL_MLX5_READY: {
		const struct flux_iok_ctrl_mlx5_ready *ready =
			(const void *)hdr;
		struct flux_iok_ctrl_hdr active = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_MLX5_ACTIVE,
			.len = sizeof(active),
		};

		if (recv_fd >= 0 || !flux_iokd_cfg.mlx5_external ||
		    hdr->len != sizeof(*ready) ||
		    flux_iokd_mlx5_client_ready(client, ready) < 0)
			return -1;
		return flux_iokd_socket_send(client->fd, &active,
					     sizeof(active), -1);
	}
	case FLUX_IOK_CTRL_MLX5_QUIESCE:
		if (recv_fd >= 0 || !flux_iokd_cfg.mlx5_external ||
		    hdr->len != sizeof(*hdr) ||
		    flux_iokd_mlx5_client_quiesce(client) < 0 ||
		    flux_iokd_socket_send(client->fd, hdr, sizeof(*hdr), -1) <
			    0)
			return -1;
		client->mlx5_clean_shutdown = true;
		return 1;
	default:
		return 0;
	}
}

static int flux_iokd_dma_map(struct flux_iokd_client *client,
			     const struct flux_iok_ctrl_dma_map *req,
			     int recv_fd)
{
	struct flux_iokd_dma_map *map;
	void *map_addr;
	void *addr;

	if (recv_fd < 0)
		return -1;

	map_addr = flux_iokd_dma_map_addr(client, req->addr, req->len);
	addr = mmap(map_addr, req->len, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED_NOREPLACE, recv_fd, 0);
	if (addr == MAP_FAILED)
		return -1;

	map = calloc(1, sizeof(*map));
	if (!map) {
		munmap(addr, req->len);
		return -1;
	}

	map->client_addr = req->addr;
	map->iokd_addr = (uintptr_t)addr;
	map->len = req->len;
	map->pgsize = req->pgsize;
#ifdef CONFIG_FLUX_FNET
	if (!flux_iokd_cfg.mlx5_external && !flux_iokd_cfg.tx_copy &&
	    flux_iok_dma_map(&flux_iokd.ctrl, addr, req->len, req->pgsize,
			     NULL) < 0) {
		munmap(addr, req->len);
		free(map);
		return -1;
	}
#endif

	map->next = client->dma_maps;
	client->dma_maps = map;
	return 0;
}

static int flux_iokd_dma_unmap(struct flux_iokd_client *client,
			       const struct flux_iok_ctrl_dma_map *req)
{
	struct flux_iokd_dma_map **link = &client->dma_maps;
	struct flux_iokd_dma_map *map;

	if (flux_iokd_mlx5_dma_range_busy(client, req->addr, req->len))
		return -1;

	while (*link) {
		if ((*link)->client_addr == req->addr &&
		    (*link)->len == req->len)
			break;
		link = &(*link)->next;
	}
	map = *link;
	if (!map)
		return 0;
	*link = map->next;

#ifdef CONFIG_FLUX_FNET
	if (!flux_iokd_cfg.mlx5_external && !flux_iokd_cfg.tx_copy)
		flux_iok_dma_unmap(&flux_iokd.ctrl, (void *)map->iokd_addr,
				   map->len, map->pgsize, NULL);
#endif
	munmap((void *)map->iokd_addr, map->len);
	free(map);
	return 0;
}

void *flux_iokd_client_control(void *arg)
{
	struct flux_iokd_client *client = arg;
	union {
		struct flux_iok_ctrl_hdr hdr;
		struct flux_iok_ctrl_dma_map dma;
		struct flux_iok_ctrl_mlx5_dma_map_done dma_done;
		struct flux_iok_ctrl_mlx5_ready ready;
	} msg;

	for (;;) {
		int recv_fd = -1;
		int ret;

		memset(&msg, 0, sizeof(msg));
		ret = flux_iokd_socket_recv(client->fd, &msg.hdr,
					    sizeof(msg.hdr), &recv_fd);
		if (ret < 0)
			goto fail_msg;
		if (msg.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
		    msg.hdr.version != FLUX_IOK_CTRL_VERSION ||
		    msg.hdr.len < sizeof(msg.hdr) || msg.hdr.len > sizeof(msg))
			goto fail_msg;
		ret = flux_iokd_socket_recv(client->fd,
					    (char *)&msg + sizeof(msg.hdr),
					    msg.hdr.len - sizeof(msg.hdr),
					    NULL);
		if (ret < 0)
			goto fail_msg;

		ret = flux_iokd_mlx5_message(client, &msg.hdr, recv_fd);
		if (ret < 0)
			goto fail_msg;
		if (ret > 0)
			break;
		if (msg.hdr.op == FLUX_IOK_CTRL_MLX5_DMA_MAP_DONE ||
		    msg.hdr.op == FLUX_IOK_CTRL_MLX5_READY)
			continue;

		if (msg.hdr.len != sizeof(msg.dma))
			goto fail_msg;
		switch (msg.hdr.op) {
		case FLUX_IOK_CTRL_DMA_MAP:
			ret = flux_iokd_dma_map(client, &msg.dma, recv_fd);
			close(recv_fd);
			recv_fd = -1;
			break;
		case FLUX_IOK_CTRL_DMA_UNMAP:
			if (recv_fd >= 0)
				goto fail_msg;
			ret = flux_iokd_dma_unmap(client, &msg.dma);
			break;
		default:
			goto fail_msg;
		}
		if (ret < 0)
			goto fail_msg;
		continue;

fail_msg:
		if (recv_fd >= 0)
			close(recv_fd);
		break;
	}

	FLUX_LOG(FLUX_LOG_WARN, "client=%d control thread exiting\n",
		 client->id);
	flux_iokd_client_request_retire(client);
	return NULL;
}
