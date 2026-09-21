#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <flux.h>
#include <kernel/asm/fnet.h>

#include "iok_client_internal.h"

#define FLUX_IOK_MLX5_OFFSET FLUX_IOK_SHARED_SIZE
#define FLUX_IOK_MLX5_SIZE (FLUX_IOK_DMA_OFFSET - FLUX_IOK_MLX5_OFFSET)
#define FLUX_IOK_MLX5_UAR_PAGE_SIZE 4096UL
#define FLUX_IOK_MLX5_READY_TIMEOUT_NS (5ULL * 1000 * 1000 * 1000)

#define FLUX_IOK_MLX5_REQUIRED_CAPS                                        \
	(FLUX_FNET_MLX5_CAP_REGULAR_RQ | FLUX_FNET_MLX5_CAP_CQE64 |        \
	 FLUX_FNET_MLX5_CAP_FIXED_RQT | FLUX_FNET_MLX5_CAP_DIRECT_HCA_VA | \
	 FLUX_FNET_MLX5_CAP_VFIO_BAR)

static bool flux_iok_mlx5_range_valid(uint64_t offset, uint64_t len,
				      uint64_t total)
{
	uint64_t end;

	return !__builtin_add_overflow(offset, len, &end) && end <= total;
}

static bool flux_iok_mlx5_power_of_two(uint32_t value)
{
	return value && !(value & (value - 1));
}

static int
flux_iok_mlx5_validate_ring(const struct flux_fnet_mlx5_ring_spec *ring,
			    uint32_t min_stride, uint32_t exact_stride,
			    uint64_t memfd_len)
{
	uint64_t bytes;

	if (!flux_iok_mlx5_power_of_two(ring->nr_entries) ||
	    ring->nr_entries > (1U << 20) ||
	    !flux_iok_mlx5_power_of_two(ring->stride) ||
	    ring->stride < min_stride ||
	    (exact_stride && ring->stride != exact_stride) ||
	    __builtin_mul_overflow((uint64_t)ring->nr_entries,
				   (uint64_t)ring->stride, &bytes) ||
	    ring->buf_offset % ring->stride ||
	    ring->dbr_offset % sizeof(uint32_t) ||
	    !flux_iok_mlx5_range_valid(ring->buf_offset, bytes, memfd_len) ||
	    !flux_iok_mlx5_range_valid(ring->dbr_offset, 2 * sizeof(uint32_t),
				       memfd_len))
		return -FLUX_EINVAL;

	return 0;
}

static int flux_iok_mlx5_validate_spec(const struct flux_fnet_mlx5_spec *spec,
				       size_t spec_len, bool require_tx_mr)
{
	uint64_t expected_len;
	uint64_t rx_bytes;
	uint64_t rx_entries;
	uint32_t rx_wq_entries = 0;
	unsigned int i;

	if (!spec || spec_len < offsetof(struct flux_fnet_mlx5_spec, queues) ||
	    spec->magic != FLUX_FNET_MLX5_SPEC_MAGIC ||
	    spec->version != FLUX_FNET_MLX5_SPEC_VERSION ||
	    spec->header_len != offsetof(struct flux_fnet_mlx5_spec, queues) ||
	    !spec->nr_queues || spec->nr_queues != (uint32_t)flux_env.nr_cpus ||
	    spec->nr_queues > FLUX_FNET_MLX5_MAX_QUEUES ||
	    __builtin_mul_overflow((uint64_t)spec->nr_queues,
				   (uint64_t)sizeof(spec->queues[0]),
				   &expected_len) ||
	    __builtin_add_overflow(expected_len, (uint64_t)spec->header_len,
				   &expected_len) ||
	    expected_len != spec_len || spec->total_len != spec_len ||
	    spec->memfd_len < spec_len ||
	    spec->memfd_len > FLUX_IOK_MLX5_SIZE ||
	    (spec->capabilities & FLUX_IOK_MLX5_REQUIRED_CAPS) !=
		    FLUX_IOK_MLX5_REQUIRED_CAPS ||
	    spec->mtu != flux_iok_client.common.mtu ||
	    !flux_iok_mlx5_range_valid(
		    spec->state_offset,
		    sizeof(struct flux_fnet_mlx5_shared_state),
		    spec->memfd_len) ||
	    !spec->rx_buf_stride || spec->rx_headroom >= spec->rx_buf_stride ||
	    !spec->rx_buf_count ||
	    __builtin_mul_overflow((uint64_t)spec->rx_buf_count,
				   (uint64_t)spec->rx_buf_stride, &rx_bytes) ||
	    rx_bytes > spec->rx_buf_len ||
	    !flux_iok_mlx5_range_valid(spec->rx_buf_offset, spec->rx_buf_len,
				       spec->memfd_len) ||
	    !spec->rx_hca_va || !spec->rx_lkey ||
	    spec->rx_mr_len < spec->rx_buf_len || !spec->bar_len ||
	    spec->bar_len > (1ULL << 30) ||
	    spec->bar_offset % FLUX_IOK_MLX5_UAR_PAGE_SIZE ||
	    spec->bar_len % FLUX_IOK_MLX5_UAR_PAGE_SIZE)
		return -FLUX_EINVAL;

	if (require_tx_mr && (spec->tx_hca_va != FLUX_MEMORY_ADDR ||
			      !spec->tx_lkey || !spec->tx_mr_len))
		return -FLUX_EINVAL;

	for (i = 0; i < spec->nr_queues; i++) {
		const struct flux_fnet_mlx5_queue_spec *q = &spec->queues[i];
		uint64_t bf_end;
		int ret;

		if (q->owner_cpu != i || !q->cqn_rx || !q->cqn_tx || !q->rqn ||
		    !q->sqn ||
		    q->bar_page_offset % FLUX_IOK_MLX5_UAR_PAGE_SIZE ||
		    !flux_iok_mlx5_range_valid(q->bar_page_offset,
					       FLUX_IOK_MLX5_UAR_PAGE_SIZE,
					       spec->bar_len) ||
		    !q->bf_size || !flux_iok_mlx5_power_of_two(q->bf_size) ||
		    q->bf_offset % sizeof(uint64_t) ||
		    __builtin_add_overflow((uint64_t)q->bf_offset,
					   (uint64_t)q->bf_size * 2, &bf_end) ||
		    bf_end > FLUX_IOK_MLX5_UAR_PAGE_SIZE)
			return -FLUX_EINVAL;

		ret = flux_iok_mlx5_validate_ring(&q->rx_cq, 64, 64,
						  spec->memfd_len);
		if (ret)
			return ret;
		ret = flux_iok_mlx5_validate_ring(&q->tx_cq, 64, 64,
						  spec->memfd_len);
		if (ret)
			return ret;
		ret = flux_iok_mlx5_validate_ring(&q->rx_wq, 16, 0,
						  spec->memfd_len);
		if (ret)
			return ret;
		ret = flux_iok_mlx5_validate_ring(&q->tx_wq, 64, 0,
						  spec->memfd_len);
		if (ret)
			return ret;

		if (!i)
			rx_wq_entries = q->rx_wq.nr_entries;
		else if (q->rx_wq.nr_entries != rx_wq_entries)
			return -FLUX_EINVAL;
	}

	if (__builtin_mul_overflow((uint64_t)spec->nr_queues,
				   (uint64_t)rx_wq_entries, &rx_entries) ||
	    rx_entries > spec->rx_buf_count)
		return -FLUX_EINVAL;

	return 0;
}

void flux_iok_mlx5_cleanup(void)
{
	if (flux_iok_client.backend.mlx5.bar &&
	    flux_iok_client.backend.mlx5.bar_len)
		munmap(flux_iok_client.backend.mlx5.bar,
		       flux_iok_client.backend.mlx5.bar_len);
	if (flux_iok_client.backend.mlx5.base &&
	    flux_iok_client.backend.mlx5.len)
		munmap(flux_iok_client.backend.mlx5.base,
		       flux_iok_client.backend.mlx5.len);

	memset(&flux_iok_client.backend.mlx5, 0,
	       sizeof(flux_iok_client.backend.mlx5));
}

static int flux_iok_mlx5_map_bar(int fd, const struct flux_fnet_mlx5_spec *spec)
{
	void *base;
	unsigned int i;

	base = mmap(NULL, spec->bar_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
		    -1, 0);
	if (base == MAP_FAILED)
		return -errno;

	for (i = 0; i < spec->nr_queues; i++) {
		uint64_t page_offset = spec->queues[i].bar_page_offset;
		uint64_t file_offset;
		void *addr;
		unsigned int j;

		for (j = 0; j < i; j++)
			if (spec->queues[j].bar_page_offset == page_offset)
				break;
		if (j != i)
			continue;
		if (__builtin_add_overflow(spec->bar_offset, page_offset,
					   &file_offset) ||
		    file_offset > (uint64_t)LLONG_MAX) {
			munmap(base, spec->bar_len);
			return -FLUX_EOVERFLOW;
		}

		addr = mmap(base + page_offset, FLUX_IOK_MLX5_UAR_PAGE_SIZE,
			    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
			    (off_t)file_offset);
		if (addr == MAP_FAILED) {
			int ret = -errno;

			munmap(base, spec->bar_len);
			return ret;
		}
	}

	flux_iok_client.backend.mlx5.bar = base;
	flux_iok_client.backend.mlx5.bar_len = spec->bar_len;
	return 0;
}

static int flux_iok_pread_all(int fd, void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = pread(fd, buf + done, len - done, (off_t)done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -FLUX_EIO;
		done += (size_t)ret;
	}
	return 0;
}

int flux_iok_mlx5_receive_prepare(void)
{
	struct flux_iok_ctrl_mlx5_prepare prepare = { 0 };
	struct flux_fnet_mlx5_shared_state *state;
	struct flux_fnet_mlx5_spec *copy = NULL;
	struct stat st;
	void *map_addr;
	int fds[2] = { -1, -1 };
	int ret;

	ret = flux_iok_socket_recv_fds(flux_iok_client.common.fd, &prepare,
				       sizeof(prepare), fds, 2);
	if (ret < 0)
		return ret;
	if (prepare.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
	    prepare.hdr.version != FLUX_IOK_CTRL_VERSION ||
	    prepare.hdr.op != FLUX_IOK_CTRL_MLX5_PREPARE ||
	    prepare.hdr.len != sizeof(prepare) ||
	    prepare.spec_len < offsetof(struct flux_fnet_mlx5_spec, queues) ||
	    prepare.spec_len >
		    offsetof(struct flux_fnet_mlx5_spec, queues) +
			    FLUX_FNET_MLX5_MAX_QUEUES *
				    sizeof(struct flux_fnet_mlx5_queue_spec) ||
	    prepare.dma_hca_va != FLUX_MEMORY_ADDR ||
	    prepare.dma_len != FLUX_IOK_DMA_SIZE) {
		ret = -FLUX_EPROTO;
		goto out;
	}

	if (fstat(fds[0], &st) < 0) {
		ret = -errno;
		goto out;
	}
	if (st.st_size < 0 || (uint64_t)st.st_size < prepare.spec_len) {
		ret = -FLUX_EINVAL;
		goto out;
	}

	copy = malloc(prepare.spec_len);
	if (!copy) {
		ret = -FLUX_ENOMEM;
		goto out;
	}
	ret = flux_iok_pread_all(fds[0], copy, prepare.spec_len);
	if (ret < 0)
		goto out;
	ret = flux_iok_mlx5_validate_spec(copy, prepare.spec_len, false);
	if (ret < 0)
		goto out;
	if ((uint64_t)st.st_size != copy->memfd_len) {
		ret = -FLUX_EINVAL;
		goto out;
	}

	map_addr = (void *)(flux_iok_client.common.window_base +
			    FLUX_IOK_MLX5_OFFSET);
	flux_iok_client.backend.mlx5.base =
		mmap(map_addr, copy->memfd_len, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_FIXED_NOREPLACE, fds[0], 0);
	if (flux_iok_client.backend.mlx5.base == MAP_FAILED) {
		flux_iok_client.backend.mlx5.base = NULL;
		ret = -errno;
		goto out;
	}
	flux_iok_client.backend.mlx5.len = copy->memfd_len;
	flux_iok_client.backend.mlx5.spec = flux_iok_client.backend.mlx5.base;

	ret = flux_iok_mlx5_validate_spec(flux_iok_client.backend.mlx5.spec,
					  prepare.spec_len, false);
	if (ret < 0)
		goto out_unmap;
	ret = flux_iok_mlx5_map_bar(fds[1], flux_iok_client.backend.mlx5.spec);
	if (ret < 0)
		goto out_unmap;

	state = flux_iok_client.backend.mlx5.base +
		flux_iok_client.backend.mlx5.spec->state_offset;
	if (__atomic_load_n(&state->generation, __ATOMIC_ACQUIRE) !=
		    flux_iok_client.backend.mlx5.spec->generation ||
	    __atomic_load_n(&state->state, __ATOMIC_ACQUIRE) <
		    FLUX_FNET_MLX5_STATE_PREPARED ||
	    __atomic_load_n(&state->state, __ATOMIC_RELAXED) >
		    FLUX_FNET_MLX5_STATE_TX_MR_READY) {
		ret = -FLUX_EPROTO;
		goto out_unmap;
	}

	flux_iok_client.backend.mlx5.state = state;
	flux_iok_client.backend.mlx5.prepared = true;
	ret = 0;
	goto out;

out_unmap:
	flux_iok_mlx5_cleanup();
out:
	free(copy);
	close(fds[0]);
	close(fds[1]);
	return ret;
}

static uint64_t flux_iok_monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

static int flux_iok_mlx5_wait_state(enum flux_fnet_mlx5_state wanted)
{
	struct flux_fnet_mlx5_shared_state *state =
		flux_iok_client.backend.mlx5.state;
	uint64_t start = flux_iok_monotonic_ns();

	if (!state || !start)
		return -FLUX_EIO;
	for (;;) {
		uint32_t current =
			__atomic_load_n(&state->state, __ATOMIC_ACQUIRE);
		uint64_t now;

		if (__atomic_load_n(&state->generation, __ATOMIC_RELAXED) !=
		    flux_iok_client.backend.mlx5.spec->generation)
			return -FLUX_EPROTO;
		if (current == (uint32_t)wanted)
			return 0;
		if (current == FLUX_FNET_MLX5_STATE_QUIESCING ||
		    current == FLUX_FNET_MLX5_STATE_DEAD)
			return -FLUX_EIO;

		now = flux_iok_monotonic_ns();
		if (!now || now - start >= FLUX_IOK_MLX5_READY_TIMEOUT_NS)
			return -FLUX_ETIMEDOUT;
		sched_yield();
	}
}

int flux_iok_mlx5_finish_dma_map(void)
{
	struct flux_iok_ctrl_mlx5_dma_map_done done = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_MLX5_DMA_MAP_DONE,
			.len = sizeof(done),
		},
		.hca_va = FLUX_MEMORY_ADDR,
		.len = flux_iok_client_kernel_dma_size(),
	};
	struct flux_iok_ctrl_mlx5_dma_map_done ack;
	int ret;

	if (!flux_iok_client.backend.mlx5.prepared)
		return -FLUX_ENODEV;
	if (!flux_iok_client.backend.mlx5.dma_done) {
		ret = flux_iok_socket_send(flux_iok_client.common.fd, &done,
					   sizeof(done), -1);
		if (ret < 0)
			return ret;
		ret = flux_iok_socket_recv(flux_iok_client.common.fd, &ack,
					   sizeof(ack), NULL);
		if (ret < 0)
			return ret;
		if (ack.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
		    ack.hdr.version != FLUX_IOK_CTRL_VERSION ||
		    ack.hdr.op != FLUX_IOK_CTRL_MLX5_DMA_MAP_DONE ||
		    ack.hdr.len != sizeof(ack) || ack.hca_va != done.hca_va ||
		    ack.len != done.len)
			return -FLUX_EPROTO;
		flux_iok_client.backend.mlx5.dma_done = true;
	}

	ret = flux_iok_mlx5_wait_state(FLUX_FNET_MLX5_STATE_TX_MR_READY);
	if (ret < 0)
		return ret;
	ret = flux_iok_mlx5_validate_spec(
		flux_iok_client.backend.mlx5.spec,
		flux_iok_client.backend.mlx5.spec->total_len, true);
	if (ret < 0)
		return ret;
	if (flux_iok_client.backend.mlx5.spec->tx_mr_len < done.len)
		return -FLUX_ERANGE;

	return 0;
}

int flux_iok_mlx5_activate(void)
{
	struct flux_fnet_mlx5_shared_state *state =
		flux_iok_client.backend.mlx5.state;
	struct flux_iok_ctrl_mlx5_ready ready;
	struct flux_iok_ctrl_hdr active;
	uint64_t expected;
	int ret;

	if (!state || !flux_iok_client.backend.mlx5.dma_done)
		return -FLUX_ENODEV;
	expected = flux_env.nr_cpus == 64 ? UINT64_MAX :
					    (1ULL << flux_env.nr_cpus) - 1;
	if (__atomic_load_n(&state->state, __ATOMIC_ACQUIRE) !=
		    FLUX_FNET_MLX5_STATE_FNET_READY ||
	    __atomic_load_n(&state->ready_queues, __ATOMIC_RELAXED) != expected)
		return -FLUX_EPROTO;

	memset(&ready, 0, sizeof(ready));
	ready.hdr.magic = FLUX_IOK_CTRL_MAGIC;
	ready.hdr.version = FLUX_IOK_CTRL_VERSION;
	ready.hdr.op = FLUX_IOK_CTRL_MLX5_READY;
	ready.hdr.len = sizeof(ready);
	ready.generation = flux_iok_client.backend.mlx5.spec->generation;
	ready.nr_queues = flux_iok_client.backend.mlx5.spec->nr_queues;
	ready.ready_queues = expected;
	ret = flux_iok_socket_send(flux_iok_client.common.fd, &ready,
				   sizeof(ready), -1);
	if (ret < 0)
		return ret;
	ret = flux_iok_socket_recv(flux_iok_client.common.fd, &active,
				   sizeof(active), NULL);
	if (ret < 0)
		return ret;
	if (active.magic != FLUX_IOK_CTRL_MAGIC ||
	    active.version != FLUX_IOK_CTRL_VERSION ||
	    active.op != FLUX_IOK_CTRL_MLX5_ACTIVE ||
	    active.len != sizeof(active))
		return -FLUX_EPROTO;
	ret = flux_iok_mlx5_wait_state(FLUX_FNET_MLX5_STATE_ACTIVE);
	if (!ret)
		flux_iok_client.backend.mlx5.active = true;
	return ret;
}

void flux_iok_mlx5_quiesce(void)
{
	struct flux_iok_ctrl_hdr req = {
		.magic = FLUX_IOK_CTRL_MAGIC,
		.version = FLUX_IOK_CTRL_VERSION,
		.op = FLUX_IOK_CTRL_MLX5_QUIESCE,
		.len = sizeof(req),
	};
	struct flux_iok_ctrl_hdr ack;
	int ret;

	if (!flux_iok_client.backend.mlx5.prepared ||
	    flux_iok_client.common.fd < 0)
		return;

	ret = flux_iok_socket_send(flux_iok_client.common.fd, &req, sizeof(req),
				   -1);
	if (!ret)
		ret = flux_iok_socket_recv(flux_iok_client.common.fd, &ack,
					   sizeof(ack), NULL);
	if (!ret &&
	    (ack.magic != FLUX_IOK_CTRL_MAGIC ||
	     ack.version != FLUX_IOK_CTRL_VERSION ||
	     ack.op != FLUX_IOK_CTRL_MLX5_QUIESCE || ack.len != sizeof(ack)))
		ret = -FLUX_EPROTO;
	if (ret < 0)
		FLUX_LOG(FLUX_LOG_WARN,
			 "mlx5 external quiesce handshake failed: %d\n", ret);
	flux_iok_client.backend.mlx5.active = false;
}

void flux_iok_mlx5_fill_netdev(struct flux_fnet_netdev *dev)
{
	dev->mlx5_base = flux_iok_client.backend.mlx5.base;
	dev->mlx5_len = flux_iok_client.backend.mlx5.len;
	dev->mlx5_bar = flux_iok_client.backend.mlx5.bar;
	dev->mlx5_bar_len = flux_iok_client.backend.mlx5.bar_len;
	dev->mlx5_spec = flux_iok_client.backend.mlx5.spec;
}
