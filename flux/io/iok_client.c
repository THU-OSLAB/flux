#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <flux.h>
#include <kernel/asm/fnet.h>

#include "iok_client_internal.h"

struct flux_iok_client_state flux_iok_client = {
	.common = {
		.fd = -1,
	},
};

static int flux_iok_requested_cpus(void)
{
	const char *value = run_cfg && run_cfg->nr_cpus ? run_cfg->nr_cpus :
						      getenv("FLUX_NR_CPUS");
	char *end;
	long nr;

	if (!value || !*value)
		return CONFIG_FLUX_MAX_CPUS;

	errno = 0;
	nr = strtol(value, &end, 10);
	if (errno || *end || nr < 1 || nr > CONFIG_FLUX_MAX_CPUS) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "invalid FLUX_NR_CPUS=%s (expected 1..%d)\n", value,
			 CONFIG_FLUX_MAX_CPUS);
		return 0;
	}

	return (int)nr;
}

static int flux_iok_parse_cpu_list(const char *value, int32_t *cpus,
				   int capacity)
{
	const char *p = value;
	int count = 0;

	if (!value || !value[0])
		return 0;
	while (*p) {
		char *end;
		long first;
		long last;

		errno = 0;
		first = strtol(p, &end, 10);
		if (errno || end == p || first < 0 || first > INT32_MAX)
			return -FLUX_EINVAL;
		last = first;
		p = end;
		if (*p == '-') {
			p++;
			errno = 0;
			last = strtol(p, &end, 10);
			if (errno || end == p || last < first || last > INT32_MAX)
				return -FLUX_EINVAL;
			p = end;
		}
		for (long cpu = first; cpu <= last; cpu++) {
			int i;
			for (i = 0; i < count; i++)
				if (cpus[i] == cpu)
				break;
			if (i < count)
				continue;
			if (count >= capacity)
				return -FLUX_EINVAL;
			cpus[count++] = (int32_t)cpu;
		}
		if (*p == ',')
			p++;
		else if (*p != '\0')
			return -FLUX_EINVAL;
	}
	return count;
}

static uint32_t flux_iok_parse_u32_setting(const char *value)
{
	char *end;
	unsigned long number;

	if (!value || !value[0])
		return 0;
	errno = 0;
	number = strtoul(value, &end, 10);
	if (errno || end == value || *end || number > UINT32_MAX)
		return 0;
	return (uint32_t)number;
}

static uint64_t flux_iok_parse_u64_setting(const char *value)
{
	char *end;
	unsigned long long number;

	if (!value || !value[0])
		return 0;
	errno = 0;
	number = strtoull(value, &end, 10);
	if (errno || end == value || *end)
		return 0;
	return (uint64_t)number;
}

static int64_t flux_iok_parse_i64_setting(const char *value)
{
	char *end;
	long long number;

	if (!value || !value[0])
		return 0;
	errno = 0;
	number = strtoll(value, &end, 10);
	if (errno || end == value || *end)
		return 0;
	return (int64_t)number;
}

static const char *flux_iok_resolve_sock_path(const char *path)
{
	static char *default_path;

	if (path && path[0] && strcmp(path, FLUX_IOK_SOCK_PATH_BASE) != 0)
		return path;

	if (!default_path)
		default_path =
			flux_user_scoped_path_strdup(FLUX_IOK_SOCK_PATH_BASE);

	return default_path ?: FLUX_IOK_SOCK_PATH_BASE;
}

static inline void flux_iok_qspec_from_out(const struct lrpc_chan_out *chan,
					   struct flux_fnet_qspec *qspec)
{
	qspec->tbl = chan->tbl;
	qspec->wb = chan->recv_head_wb;
	qspec->size = chan->size;
}

static inline void flux_iok_qspec_from_in(const struct lrpc_chan_in *chan,
					  struct flux_fnet_qspec *qspec)
{
	qspec->tbl = chan->tbl;
	qspec->wb = chan->recv_head_wb;
	qspec->size = chan->size;
}

static int flux_memfd_create(const char *name, unsigned int flags)
{
#ifdef SYS_memfd_create
	int fd;

	fd = syscall(SYS_memfd_create, name, flags);
	if (fd >= 0 || errno != FLUX_ENOSYS)
		return fd;
#endif

	/*
	 * Some host environments running flux still reject memfd_create()
	 * even though regular tmpfs-backed shared files are available.
	 * Fall back to an unlinked file in tmpfs so the fd can still be
	 * passed to flux_iokd and mapped on both sides.
	 */
	{
		static const char *dirs[] = { "/dev/shm", "/tmp" };
		char path[256];
		int i;

		for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
			int attempt;

			for (attempt = 0; attempt < 32; attempt++) {
				int open_flags = O_RDWR | O_CREAT | O_EXCL;
				int tmpfd;

				if (flags & MFD_CLOEXEC)
					open_flags |= O_CLOEXEC;

				snprintf(path, sizeof(path), "%s/%s.%ld.%d",
					 dirs[i], name, (long)getpid(),
					 attempt);
				tmpfd = open(path, open_flags, 0600);
				if (tmpfd < 0) {
					if (errno == FLUX_EEXIST)
						continue;
					break;
				}

				unlink(path);
				if (!(flags & MFD_CLOEXEC))
					fcntl(tmpfd, F_SETFD, 0);
				return tmpfd;
			}

			FLUX_LOG(
				FLUX_LOG_WARN,
				"tmpfs fallback create failed in %s for %s: %s\n",
				dirs[i], name, strerror(errno));
		}
	}

	errno = FLUX_ENOSYS;
	return -1;
}

static size_t flux_iok_lrpc_shm_size(void)
{
#ifdef CONFIG_FLUX_FNET
	size_t ret = 0;
	size_t q;

	q = sizeof(struct lrpc_msg) * FLUX_FNET_LRPC_QUEUE_SIZE;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * flux_env.nr_cpus * 3;

	return ret;
#else
	return 0;
#endif
}

static size_t flux_iok_shared_size(void)
{
	size_t lrpc_len = 0;
	size_t timer_len;

	if (flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC)
		lrpc_len = align_up(flux_iok_lrpc_shm_size(), PGSIZE_2MB);
	timer_len = align_up(sizeof(struct flux_iok_timer_entry) *
				     (size_t)flux_env.nr_cpus,
			     CACHE_LINE_SIZE);
	return lrpc_len + timer_len;
}

static inline void flux_iok_lrpc_alloc_queue(void *base, size_t *offset,
					     struct lrpc_msg **tbl_out,
					     uint32_t **wb_out)
{
	*offset = align_up(*offset, CACHE_LINE_SIZE);

	*tbl_out = (struct lrpc_msg *)(base + *offset);
	*offset += align_up(sizeof(struct lrpc_msg) * FLUX_FNET_LRPC_QUEUE_SIZE,
			    CACHE_LINE_SIZE);

	*wb_out = (uint32_t *)(base + *offset);
	*offset += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
}

static int flux_iok_client_init_queues(void *base)
{
#ifdef CONFIG_FLUX_FNET
	struct flux_iok_client_pcpu *pcpu;
	struct lrpc_msg *tbl;
	uint32_t *wb;
	size_t offset = 0;
	int i;

	if (flux_iok_client.mode != FLUX_IOK_BACKEND_LRPC)
		return 0;

	flux_iok_client.backend.lrpc.pcpus =
		calloc((size_t)flux_env.nr_cpus,
		       sizeof(*flux_iok_client.backend.lrpc.pcpus));
	if (!flux_iok_client.backend.lrpc.pcpus)
		return -FLUX_ENOMEM;

	flux_iok_client.backend.lrpc.base = base;
	flux_iok_client.backend.lrpc.len =
		align_up(flux_iok_lrpc_shm_size(), PGSIZE_2MB);

	for (i = 0; i < flux_env.nr_cpus; i++) {
		pcpu = &flux_iok_client.backend.lrpc.pcpus[i];

		flux_iok_lrpc_alloc_queue(base, &offset, &tbl, &wb);
		lrpc_init_out(&pcpu->rxq, tbl, FLUX_FNET_LRPC_QUEUE_SIZE, wb);

		flux_iok_lrpc_alloc_queue(base, &offset, &tbl, &wb);
		lrpc_init_in(&pcpu->txpktq, tbl, FLUX_FNET_LRPC_QUEUE_SIZE, wb);

		flux_iok_lrpc_alloc_queue(base, &offset, &tbl, &wb);
		lrpc_init_in(&pcpu->txcmdq, tbl, FLUX_FNET_LRPC_QUEUE_SIZE, wb);
	}

	return 0;
#else
	return 0;
#endif
}

static int flux_iok_client_register_shm(int memfd)
{
	struct flux_iok_ctrl_register req = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_REGISTER,
			.len = sizeof(req),
		},
		.nr_cpus = flux_env.nr_cpus,
		.shm_base = (uintptr_t)flux_iok_client.common.shm_base,
		.shm_len = flux_iok_client.common.shm_len,
		.timer_base = (uintptr_t)flux_iok_client.common.timer_base,
		.timer_len = flux_iok_client.common.timer_len,
	};

#ifdef CONFIG_FLUX_FNET
	int i;

	if (flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC) {
		req.rx_base = (uintptr_t)flux_iok_client.backend.lrpc.rx_base;
		req.rx_len = flux_iok_client.backend.lrpc.rx_len;
	}

	for (i = 0; i < flux_env.nr_cpus &&
		    flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC;
	     i++) {
		flux_iok_qspec_from_out(
			&flux_iok_client.backend.lrpc.pcpus[i].rxq,
			&req.rxqs[i]);
		flux_iok_qspec_from_in(
			&flux_iok_client.backend.lrpc.pcpus[i].txpktq,
			&req.txpktqs[i]);
		flux_iok_qspec_from_in(
			&flux_iok_client.backend.lrpc.pcpus[i].txcmdq,
			&req.txcmdqs[i]);
	}
#endif

	return flux_iok_socket_send(flux_iok_client.common.fd, &req,
				    sizeof(req), memfd);
}

static int flux_iok_client_connect(void)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	const char *path = flux_iok_sock_path();
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path))
		return -FLUX_EINVAL;

	memcpy(addr.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "connect(%s) failed: %s\n", path,
			 strerror(errno));
		close(fd);
		return -errno;
	}

	return fd;
}

const char *flux_iok_sock_path(void)
{
	if (run_cfg && run_cfg->iok_sock_path && run_cfg->iok_sock_path[0])
		return flux_iok_resolve_sock_path(run_cfg->iok_sock_path);

	return flux_iok_resolve_sock_path(NULL);
}

bool flux_iok_client_is_enabled(void)
{
	return flux_iok_client.common.ready;
}

bool flux_iok_client_has_network(void)
{
	return flux_iok_client.common.net_enabled;
}

size_t flux_iok_client_kernel_dma_size(void)
{
	size_t size = flux_env.dma_size;

	if (size < 256 * MB)
		size = 256 * MB;
	return align_up(size, 256 * MB);
}

static int flux_iok_dma_allocator_init(void)
{
	size_t kernel_dma_size = flux_iok_client_kernel_dma_size();

	if (kernel_dma_size > FLUX_IOK_DMA_SIZE) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"iok kernel dma window too large len=%lu dma_window=%lu\n",
			(unsigned long)kernel_dma_size,
			(unsigned long)FLUX_IOK_DMA_SIZE);
		return -FLUX_EOVERFLOW;
	}

	flux_iok_client.common.dma_next_addr =
		flux_iok_client.common.window_base + FLUX_IOK_DMA_OFFSET +
		kernel_dma_size;
	return 0;
}

static uintptr_t flux_iok_dma_alloc_addr(size_t len)
{
	uintptr_t addr;
	uintptr_t next;
	uintptr_t end;

	len = align_up(len, PGSIZE_2MB);
	end = flux_iok_client.common.window_base + FLUX_IOK_DMA_OFFSET +
	      FLUX_IOK_DMA_SIZE;

	for (;;) {
		addr = atomic_load_relaxed(
			&flux_iok_client.common.dma_next_addr);
		next = addr + len;
		if (next > end)
			return 0;
		if (atomic_cmpxchg_relaxed(
			    &flux_iok_client.common.dma_next_addr, &addr, next))
			return addr;
	}
}

void *flux_iok_client_dma_alloc(void *hint, size_t len, size_t pgsize, int node)
{
	struct flux_iok_ctrl_dma_map req = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_DMA_MAP,
			.len = sizeof(req),
		},
		.len = align_up(len, pgsize),
		.pgsize = pgsize,
	};
	void *addr = MAP_FAILED;
	uintptr_t map_addr;
	unsigned int flags = MFD_CLOEXEC;
	int fd = -1;

	if (!flux_iok_client.common.ready) {
		errno = FLUX_ENODEV;
		return MAP_FAILED;
	}

	if (hint) {
		map_addr = (uintptr_t)hint;
	} else {
		map_addr = flux_iok_dma_alloc_addr(req.len);
		if (!map_addr) {
			errno = FLUX_ENOMEM;
			FLUX_LOG(
				FLUX_LOG_ERR,
				"iok dma window exhausted len=%lu base=%#lx next=%#lx\n",
				(unsigned long)req.len,
				(unsigned long)
					flux_iok_client.common.window_base,
				(unsigned long)atomic_load_relaxed(
					&flux_iok_client.common.dma_next_addr));
			return MAP_FAILED;
		}
	}

#ifdef MFD_HUGETLB
	if (pgsize >= PGSIZE_2MB)
		flags |= MFD_HUGETLB;
#endif
	fd = flux_memfd_create(FLUX_IOKD_NAME_PREFIX "_dma", flags);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to create iok dma fd len=%lu: %s\n",
			 (unsigned long)req.len, strerror(errno));
		goto fail_addr;
	}

	if (ftruncate(fd, req.len) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "ftruncate iok dma fd len=%lu failed: %s\n",
			 (unsigned long)req.len, strerror(errno));
		goto fail_fd;
	}

	addr = mmap((void *)map_addr, req.len, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "mmap iok dma failed addr=%#lx len=%lu: %s\n",
			 (unsigned long)map_addr, (unsigned long)req.len,
			 strerror(errno));
		goto fail_fd;
	}

	req.addr = (uintptr_t)addr;
	if (flux_iok_socket_send(flux_iok_client.common.fd, &req, sizeof(req),
				 fd) < 0) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"failed to send iok dma map request addr=%#lx len=%lu\n",
			(unsigned long)req.addr, (unsigned long)req.len);
		goto fail_map;
	}

	close(fd);
	return addr;

fail_map:
	munmap(addr, req.len);
	addr = MAP_FAILED;
fail_fd:
	close(fd);
fail_addr:
	return addr;
}

void flux_iok_client_dma_free(void *addr, size_t len, size_t pgsize)
{
	struct flux_iok_ctrl_dma_map req = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_DMA_UNMAP,
			.len = sizeof(req),
		},
		.addr = (uintptr_t)addr,
		.len = align_up(len, pgsize),
		.pgsize = pgsize,
	};

	if (!addr)
		return;

	if (flux_iok_client.common.ready)
		flux_iok_socket_send(flux_iok_client.common.fd, &req,
				     sizeof(req), -1);

	munmap(addr, req.len);
}

int flux_iok_client_activate_netdev(void)
{
	if (flux_iok_client.mode != FLUX_IOK_BACKEND_MLX5)
		return 0;
	return flux_iok_mlx5_activate();
}

int flux_iok_client_init(void)
{
	int cpu_list_count;
	struct flux_iok_ctrl_attach attach = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_ATTACH,
			.len = sizeof(attach),
		},
		.requested_cpus = flux_iok_requested_cpus(),
		.ip_addr = run_cfg->nic_ip_addr ? ntohl(inet_addr(run_cfg->nic_ip_addr)) : 0,
		.netmask = run_cfg->nic_ip_mask ? ntohl(inet_addr(run_cfg->nic_ip_mask)) : 0,
		.gateway = run_cfg->nic_ip_gw ? ntohl(inet_addr(run_cfg->nic_ip_gw)) : 0,
		.io_weight = flux_iok_parse_u32_setting(run_cfg->blkio_weight),
		.net_class_id =
			flux_iok_parse_u32_setting(run_cfg->network_class_id),
		.net_priority =
			flux_iok_parse_u32_setting(run_cfg->network_priority),
		.cpu_shares = flux_iok_parse_u32_setting(run_cfg->cpu_shares),
		.cpu_quota = flux_iok_parse_i64_setting(run_cfg->cpu_quota),
		.cpu_period = flux_iok_parse_u64_setting(run_cfg->cpu_period),
	};
	struct flux_iok_ctrl_ack ack = { 0 };
	size_t lrpc_len;
	bool rx_mapped = false;
	int memfd = -1, fd, ret;

	if (attach.requested_cpus == 0)
		return -FLUX_EINVAL;
	cpu_list_count = flux_iok_parse_cpu_list(
		run_cfg->cpu_set, attach.cpu_list, CONFIG_FLUX_MAX_CPUS);
	if (cpu_list_count < 0)
		return cpu_list_count;
	if (cpu_list_count > 0) {
		if ((uint32_t)cpu_list_count != attach.requested_cpus)
			return -FLUX_EINVAL;
		attach.flags |= FLUX_IOK_ATTACH_F_CPU_LIST;
	}

	fd = flux_iok_client_connect();
	if (fd < 0)
		return fd;

	ret = flux_iok_socket_send(fd, &attach, sizeof(attach), -1);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to send attach to flux_iokd: %d\n", ret);
		goto fail_fd;
	}

	ret = flux_iok_socket_recv(fd, &ack, sizeof(ack), NULL);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to receive ack from flux_iokd: %d\n", ret);
		goto fail_fd;
	}

	if (ack.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
	    ack.hdr.version != FLUX_IOK_CTRL_VERSION ||
	    ack.hdr.op != FLUX_IOK_CTRL_ACK || ack.hdr.len != sizeof(ack)) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"invalid flux_iokd ack hdr: magic=%#x version=%u op=%u len=%u\n",
			ack.hdr.magic, ack.hdr.version, ack.hdr.op,
			ack.hdr.len);
		ret = -FLUX_EIO;
		goto fail_fd;
	}

	memfd = flux_memfd_create(FLUX_IOKD_NAME_PREFIX "_client", MFD_CLOEXEC);
	if (memfd < 0) {
		ret = -errno;
		FLUX_LOG(FLUX_LOG_ERR,
			 "memfd_create for iok client shm failed: %s\n",
			 strerror(errno));
		goto fail_fd;
	}

	flux_iok_client.common.fd = fd;
	flux_iok_client.common.client_id = (int)ack.client_id;
	flux_iok_client.common.mtu = ack.mtu;
	flux_iok_client.common.csum_offload = (int)ack.csum_offload;
	flux_iok_client.common.net_enabled = attach.ip_addr != 0;
	flux_iok_client.common.window_base = ack.window_base;
	switch (ack.backend) {
	case FLUX_IOK_NET_BACKEND_LRPC:
		flux_iok_client.mode = FLUX_IOK_BACKEND_LRPC;
		break;
	case FLUX_IOK_NET_BACKEND_MLX5_EXTERNAL:
		if (ack.rx_shm_key || ack.rx_len) {
			ret = -FLUX_EPROTO;
			FLUX_LOG(
				FLUX_LOG_ERR,
				"mlx5 external ACK includes legacy RX memory\n");
			goto fail_memfd;
		}
		flux_iok_client.mode = attach.ip_addr ? FLUX_IOK_BACKEND_MLX5 :
							FLUX_IOK_BACKEND_NONE;
		break;
	default:
		ret = -FLUX_EPROTO;
		FLUX_LOG(FLUX_LOG_ERR,
			 "invalid network backend in flux_iokd ACK: %u\n",
			 ack.backend);
		goto fail_memfd;
	}
	memcpy(flux_iok_client.common.host_mac, ack.host_mac,
	       sizeof(flux_iok_client.common.host_mac));

	if (ack.nr_cpus == 0 || ack.nr_cpus > (uint32_t)attach.requested_cpus ||
	    ack.nr_cpus > CONFIG_FLUX_MAX_CPUS) {
		ret = -FLUX_EINVAL;
		FLUX_LOG(
			FLUX_LOG_ERR,
			"invalid cpu assignment from flux_iokd: requested=%u assigned=%u\n",
			attach.requested_cpus, ack.nr_cpus);
		goto fail_memfd;
	}

	ret = flux_env_set_cpus(ack.cpu_list, (int)ack.nr_cpus);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to apply cpu assignment from flux_iokd: %d\n",
			 ret);
		goto fail_memfd;
	}

	flux_iok_client.common.shm_len = flux_iok_shared_size();

	if (ftruncate(memfd, flux_iok_client.common.shm_len) < 0) {
		ret = -errno;
		FLUX_LOG(FLUX_LOG_ERR,
			 "ftruncate for iok client shm failed len=%zu: %s\n",
			 flux_iok_client.common.shm_len, strerror(errno));
		goto fail_memfd;
	}

	flux_iok_client.common.shm_base =
		mmap((void *)(flux_iok_client.common.window_base +
			      FLUX_IOK_SHARED_OFFSET),
		     flux_iok_client.common.shm_len, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_FIXED_NOREPLACE, memfd, 0);
	if (flux_iok_client.common.shm_base == MAP_FAILED) {
		ret = -errno;
		FLUX_LOG(
			FLUX_LOG_ERR,
			"mmap iok shared window failed addr=%#lx len=%zu: %s\n",
			(unsigned long)(flux_iok_client.common.window_base +
					FLUX_IOK_SHARED_OFFSET),
			flux_iok_client.common.shm_len, strerror(errno));
		goto fail_memfd;
	}

	lrpc_len = flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC ?
			   align_up(flux_iok_lrpc_shm_size(), PGSIZE_2MB) :
			   0;
	flux_iok_client.common.timer_base =
		flux_iok_client.common.shm_base + lrpc_len;
	flux_iok_client.common.timer_len = align_up(
		sizeof(struct flux_iok_timer_entry) * (size_t)flux_env.nr_cpus,
		CACHE_LINE_SIZE);

#ifdef CONFIG_FLUX_FNET
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC)
		flux_iok_client.backend.lrpc.rx_len = ack.rx_len;
	if (attach.ip_addr != 0 &&
	    flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC) {
		if (!ack.rx_shm_key || !flux_iok_client.backend.lrpc.rx_len ||
		    ack.rx_pgsize != PGSIZE_2MB) {
			ret = -FLUX_ENODEV;
			FLUX_LOG(FLUX_LOG_ERR,
				 "flux_iokd has no usable network dataplane\n");
			goto fail_map;
		}

		flux_iok_client.backend.lrpc.rx_base =
			flux_mem_map_shm((mem_key_t)ack.rx_shm_key, NULL,
					 flux_iok_client.backend.lrpc.rx_len,
					 (size_t)ack.rx_pgsize, false);
		if (flux_iok_client.backend.lrpc.rx_base == MAP_FAILED) {
			ret = -errno;
			flux_iok_client.backend.lrpc.rx_base = NULL;
			FLUX_LOG(
				FLUX_LOG_ERR,
				"attach shared rx shm failed key=%u len=%zu: %s\n",
				ack.rx_shm_key,
				flux_iok_client.backend.lrpc.rx_len,
				strerror(errno));
			goto fail_map;
		}
		rx_mapped = true;
	}
#endif

	ret = flux_iok_client_init_queues(flux_iok_client.common.shm_base);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to initialize iok queues: %d\n",
			 ret);
		goto fail_map;
	}

	memset(flux_iok_client.common.timer_base, 0,
	       flux_iok_client.common.timer_len);

	ret = flux_iok_client_register_shm(memfd);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to register iok shared memory: %d\n", ret);
		goto fail_map;
	}
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5) {
		ret = flux_iok_mlx5_receive_prepare();
		if (ret < 0) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"failed to receive mlx5 external resources: %d\n",
				ret);
			goto fail_map;
		}
	}

	ret = flux_iok_dma_allocator_init();
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to initialize iok dma allocator: %d\n", ret);
		goto fail_map;
	}

	flux_iok_client.common.ready = true;
	close(memfd);
	return 0;

fail_map:
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5) {
		flux_iok_mlx5_cleanup();
	} else if (flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC) {
		free(flux_iok_client.backend.lrpc.pcpus);
		flux_iok_client.backend.lrpc.pcpus = NULL;
		if (rx_mapped && flux_iok_client.backend.lrpc.rx_base &&
		    flux_iok_client.backend.lrpc.rx_len)
			flux_mem_unmap_shm(
				flux_iok_client.backend.lrpc.rx_base);
		flux_iok_client.backend.lrpc.rx_base = NULL;
		flux_iok_client.backend.lrpc.rx_len = 0;
	}
	munmap(flux_iok_client.common.shm_base, flux_iok_client.common.shm_len);
	flux_iok_client.common.shm_base = NULL;
	flux_iok_client.common.timer_base = NULL;
	flux_iok_client.common.timer_len = 0;
	flux_iok_client.common.net_enabled = false;
	flux_iok_client.mode = FLUX_IOK_BACKEND_NONE;
fail_memfd:
	close(memfd);
fail_fd:
	close(fd);
	flux_iok_client.common.fd = -1;
	flux_iok_client.common.net_enabled = false;
	flux_iok_client.mode = FLUX_IOK_BACKEND_NONE;
	return ret;
}

void flux_iok_client_fini(void)
{
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5)
		flux_iok_mlx5_quiesce();
	flux_iok_client.common.ready = false;
	if (flux_iok_client.common.fd >= 0)
		close(flux_iok_client.common.fd);
	flux_iok_client.common.fd = -1;
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5)
		flux_iok_mlx5_cleanup();
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC &&
	    flux_iok_client.backend.lrpc.rx_base &&
	    flux_iok_client.backend.lrpc.rx_len)
		flux_mem_unmap_shm(flux_iok_client.backend.lrpc.rx_base);
	if (flux_iok_client.common.shm_base)
		munmap(flux_iok_client.common.shm_base,
		       flux_iok_client.common.shm_len);
	flux_iok_client.common.shm_base = NULL;
	flux_iok_client.common.timer_base = NULL;
	flux_iok_client.common.timer_len = 0;
	flux_iok_client.common.net_enabled = false;
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_LRPC)
		free(flux_iok_client.backend.lrpc.pcpus);
	memset(&flux_iok_client.backend, 0, sizeof(flux_iok_client.backend));
	flux_iok_client.mode = FLUX_IOK_BACKEND_NONE;
}

int flux_iok_client_fill_netdev(struct flux_fnet_netdev *dev)
{
	int i;

	if (!flux_iok_client.common.ready)
		return -FLUX_ENODEV;
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5) {
		int ret = flux_iok_mlx5_finish_dma_map();

		if (ret < 0)
			return ret;
	} else if (flux_iok_client.mode != FLUX_IOK_BACKEND_LRPC) {
		return -FLUX_ENODEV;
	}

	dev->nb_rx_queues = flux_env.nr_cpus;
	dev->nb_tx_queues = flux_env.nr_cpus;
	dev->mtu = flux_iok_client.common.mtu;
	dev->mode = flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5 ?
			    FLUX_FNET_MODE_MLX5_EXTERNAL :
			    FLUX_FNET_MODE_KERNEL;
	dev->csum_offload = flux_iok_client.common.csum_offload;
	dev->gso_offload = false;
	dev->tx_mempool = NULL;
	dev->addr = run_cfg->nic_ip_addr ?
			    ntohl(inet_addr(run_cfg->nic_ip_addr)) :
			    0;
	dev->netmask = run_cfg->nic_ip_mask ?
			       ntohl(inet_addr(run_cfg->nic_ip_mask)) :
			       0;
	dev->gateway =
		run_cfg->nic_ip_gw ? ntohl(inet_addr(run_cfg->nic_ip_gw)) : 0;
	memcpy(dev->eth_addr, flux_iok_client.common.host_mac,
	       sizeof(dev->eth_addr));
	if (flux_iok_client.mode == FLUX_IOK_BACKEND_MLX5) {
		flux_iok_mlx5_fill_netdev(dev);
		return 0;
	}

	dev->rx_base = flux_iok_client.backend.lrpc.rx_base;
	dev->rx_len = flux_iok_client.backend.lrpc.rx_len;

	dev->rxqs = calloc((size_t)flux_env.nr_cpus, sizeof(*dev->rxqs));
	dev->txpktqs = calloc((size_t)flux_env.nr_cpus, sizeof(*dev->txpktqs));
	dev->txcmdqs = calloc((size_t)flux_env.nr_cpus, sizeof(*dev->txcmdqs));
	if (!dev->rxqs || !dev->txpktqs || !dev->txcmdqs) {
		free(dev->rxqs);
		free(dev->txpktqs);
		free(dev->txcmdqs);
		dev->rxqs = NULL;
		dev->txpktqs = NULL;
		dev->txcmdqs = NULL;
		return -FLUX_ENOMEM;
	}

	for (i = 0; i < flux_env.nr_cpus; i++) {
		flux_iok_qspec_from_out(
			&flux_iok_client.backend.lrpc.pcpus[i].rxq,
			&dev->rxqs[i]);
		flux_iok_qspec_from_in(
			&flux_iok_client.backend.lrpc.pcpus[i].txpktq,
			&dev->txpktqs[i]);
		flux_iok_qspec_from_in(
			&flux_iok_client.backend.lrpc.pcpus[i].txcmdq,
			&dev->txcmdqs[i]);
	}

	return 0;
}

void *flux_iok_client_timer_alloc(int cpu, bool oneshot)
{
	struct flux_iok_timer_entry *timers = flux_iok_client.common.timer_base;

	if (!flux_iok_client.common.ready || !timers || cpu < 0 ||
	    cpu >= flux_env.nr_cpus || !oneshot)
		return NULL;

	return &timers[cpu];
}

int flux_iok_client_timer_set_oneshot(void *timer, unsigned long ns)
{
	struct flux_iok_timer_entry *entry = timer;
	uint64_t deadline;

	if (!entry)
		return -FLUX_EINVAL;

	/*
	 * ULONG_MAX claims a delivery generated by iokd. Only clear an entry
	 * while it still carries the delivery-pending bit: a delayed duplicate
	 * UINTR must not erase a newer deadline installed by Flux.
	 */
	if (ns == ~0UL) {
		deadline = atomic_load_acquire(&entry->deadline_ns);
		while (deadline & FLUX_IOK_TIMER_DELIVERY_PENDING) {
			if (atomic_cmpxchg_acq_rel_acquire(&entry->deadline_ns,
							   &deadline, 0))
				return 1;
		}
		return 0;
	}

	/* A zero delta is an unconditional clock-event disarm operation. */
	deadline = ns ? now_ns() + ns : 0;
	atomic_store_release(&entry->deadline_ns, deadline);
	return 0;
}
