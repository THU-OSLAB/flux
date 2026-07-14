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

#include "iok_client.h"

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif

struct flux_iok_client_state {
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
	void *rx_base;
	size_t rx_len;
	void *lrpc_base;
	size_t lrpc_len;
	unsigned int mtu;
	int csum_offload;
	unsigned char host_mac[6];
	struct flux_iok_client_pcpu *pcpus;
};

static struct flux_iok_client_state flux_iok_client = {
	.fd = -1,
};

static int flux_iok_requested_cpus(void)
{
	return CONFIG_FLUX_MAX_CPUS;
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

static int flux_iok_send_msg(int fd, const void *buf, size_t len, int send_fd)
{
	struct msghdr msg = { 0 };
	struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (send_fd >= 0) {
		memset(cmsgbuf, 0, sizeof(cmsgbuf));
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(int));
	}

	if (sendmsg(fd, &msg, MSG_NOSIGNAL) != (ssize_t)len)
		return -errno;

	return 0;
}

static int flux_iok_recv_msg(int fd, void *buf, size_t len, int *recv_fd)
{
	struct msghdr msg = { 0 };
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = len,
	};
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	ssize_t ret;

	if (recv_fd)
		*recv_fd = -1;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	ret = recvmsg(fd, &msg, MSG_WAITALL);
	if (ret != (ssize_t)len)
		return ret < 0 ? -errno : -FLUX_EIO;

	if (!recv_fd)
		return 0;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_RIGHTS)
			continue;
		memcpy(recv_fd, CMSG_DATA(cmsg), sizeof(int));
		break;
	}

	return 0;
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
	size_t lrpc_len;
	size_t rx_len = 0;
	size_t timer_len;

	lrpc_len = align_up(flux_iok_lrpc_shm_size(), PGSIZE_2MB);
#ifdef CONFIG_FLUX_FNET
	rx_len = 0;
#endif
	timer_len = align_up(sizeof(struct flux_iok_timer_entry) *
				     (size_t)flux_env.nr_cpus,
			     CACHE_LINE_SIZE);
	return lrpc_len + rx_len + timer_len;
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

	flux_iok_client.pcpus = calloc((size_t)flux_env.nr_cpus,
				       sizeof(*flux_iok_client.pcpus));
	if (!flux_iok_client.pcpus)
		return -FLUX_ENOMEM;

	flux_iok_client.lrpc_base = base;
	flux_iok_client.lrpc_len =
		align_up(flux_iok_lrpc_shm_size(), PGSIZE_2MB);

	for (i = 0; i < flux_env.nr_cpus; i++) {
		pcpu = &flux_iok_client.pcpus[i];

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
		.shm_base = (uintptr_t)flux_iok_client.shm_base,
		.shm_len = flux_iok_client.shm_len,
		.timer_base = (uintptr_t)flux_iok_client.timer_base,
		.timer_len = flux_iok_client.timer_len,
		.rx_base = (uintptr_t)flux_iok_client.rx_base,
		.rx_len = flux_iok_client.rx_len,
	};

#ifdef CONFIG_FLUX_FNET
	int i;

	for (i = 0; i < flux_env.nr_cpus; i++) {
		flux_iok_qspec_from_out(&flux_iok_client.pcpus[i].rxq,
					&req.rxqs[i]);
		flux_iok_qspec_from_in(&flux_iok_client.pcpus[i].txpktq,
				       &req.txpktqs[i]);
		flux_iok_qspec_from_in(&flux_iok_client.pcpus[i].txcmdq,
				       &req.txcmdqs[i]);
	}
#endif

	return flux_iok_send_msg(flux_iok_client.fd, &req, sizeof(req), memfd);
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
	return flux_iok_client.ready;
}

bool flux_iok_client_has_network(void)
{
	return flux_iok_client.net_enabled;
}

static int flux_iok_dma_allocator_init(void)
{
	size_t kernel_dma_size = flux_env.dma_size;

	if (kernel_dma_size < 256 * MB)
		kernel_dma_size = 256 * MB;
	kernel_dma_size = align_up(kernel_dma_size, 256 * MB);
	if (kernel_dma_size > FLUX_IOK_DMA_SIZE) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"iok kernel dma window too large len=%lu dma_window=%lu\n",
			(unsigned long)kernel_dma_size,
			(unsigned long)FLUX_IOK_DMA_SIZE);
		return -FLUX_EOVERFLOW;
	}

	flux_iok_client.dma_next_addr = flux_iok_client.window_base +
					FLUX_IOK_DMA_OFFSET + kernel_dma_size;
	return 0;
}

static uintptr_t flux_iok_dma_alloc_addr(size_t len)
{
	uintptr_t addr;
	uintptr_t next;
	uintptr_t end;

	len = align_up(len, PGSIZE_2MB);
	end = flux_iok_client.window_base + FLUX_IOK_DMA_OFFSET +
	      FLUX_IOK_DMA_SIZE;

	for (;;) {
		addr = atomic_load_relaxed(&flux_iok_client.dma_next_addr);
		next = addr + len;
		if (next > end)
			return 0;
		if (atomic_cmpxchg_relaxed(&flux_iok_client.dma_next_addr,
					   &addr, next))
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

	if (!flux_iok_client.ready) {
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
				(unsigned long)flux_iok_client.window_base,
				(unsigned long)atomic_load_relaxed(
					&flux_iok_client.dma_next_addr));
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
	if (flux_iok_send_msg(flux_iok_client.fd, &req, sizeof(req), fd) < 0) {
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

	if (flux_iok_client.ready)
		flux_iok_send_msg(flux_iok_client.fd, &req, sizeof(req), -1);

	munmap(addr, req.len);
}

int flux_iok_client_init(void)
{
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
	};
	struct flux_iok_ctrl_ack ack = { 0 };
	size_t lrpc_len;
	bool rx_mapped = false;
	int memfd = -1, fd, ret;

	fd = flux_iok_client_connect();
	if (fd < 0)
		return fd;

	ret = flux_iok_send_msg(fd, &attach, sizeof(attach), -1);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to send attach to flux_iokd: %d\n", ret);
		goto fail_fd;
	}

	ret = flux_iok_recv_msg(fd, &ack, sizeof(ack), NULL);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to receive ack from flux_iokd: %d\n", ret);
		goto fail_fd;
	}

	if (ack.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
	    ack.hdr.version != FLUX_IOK_CTRL_VERSION ||
	    ack.hdr.op != FLUX_IOK_CTRL_ACK) {
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

	flux_iok_client.fd = fd;
	flux_iok_client.client_id = (int)ack.client_id;
	flux_iok_client.mtu = ack.mtu;
	flux_iok_client.csum_offload = (int)ack.csum_offload;
	flux_iok_client.net_enabled = attach.ip_addr != 0;
	flux_iok_client.window_base = ack.window_base;
	flux_iok_client.rx_len = ack.rx_len;
	memcpy(flux_iok_client.host_mac, ack.host_mac,
	       sizeof(flux_iok_client.host_mac));

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

	flux_iok_client.shm_len = flux_iok_shared_size();

	if (ftruncate(memfd, flux_iok_client.shm_len) < 0) {
		ret = -errno;
		FLUX_LOG(FLUX_LOG_ERR,
			 "ftruncate for iok client shm failed len=%zu: %s\n",
			 flux_iok_client.shm_len, strerror(errno));
		goto fail_memfd;
	}

	flux_iok_client.shm_base = mmap(
		(void *)(flux_iok_client.window_base + FLUX_IOK_SHARED_OFFSET),
		flux_iok_client.shm_len, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_FIXED_NOREPLACE, memfd, 0);
	if (flux_iok_client.shm_base == MAP_FAILED) {
		ret = -errno;
		FLUX_LOG(
			FLUX_LOG_ERR,
			"mmap iok shared window failed addr=%#lx len=%zu: %s\n",
			(unsigned long)(flux_iok_client.window_base +
					FLUX_IOK_SHARED_OFFSET),
			flux_iok_client.shm_len, strerror(errno));
		goto fail_memfd;
	}

	lrpc_len = align_up(flux_iok_lrpc_shm_size(), PGSIZE_2MB);
	flux_iok_client.timer_base = flux_iok_client.shm_base + lrpc_len;
	flux_iok_client.timer_len = align_up(
		sizeof(struct flux_iok_timer_entry) * (size_t)flux_env.nr_cpus,
		CACHE_LINE_SIZE);

#ifdef CONFIG_FLUX_FNET
	flux_iok_client.rx_len = ack.rx_len;
	if (attach.ip_addr != 0) {
		if (!ack.rx_shm_key || !flux_iok_client.rx_len ||
		    ack.rx_pgsize != PGSIZE_2MB) {
			ret = -FLUX_ENODEV;
			FLUX_LOG(FLUX_LOG_ERR,
				 "flux_iokd has no usable network dataplane\n");
			goto fail_map;
		}

		flux_iok_client.rx_base =
			flux_mem_map_shm((mem_key_t)ack.rx_shm_key, NULL,
					 flux_iok_client.rx_len,
					 (size_t)ack.rx_pgsize, false);
		if (flux_iok_client.rx_base == MAP_FAILED) {
			ret = -errno;
			flux_iok_client.rx_base = NULL;
			FLUX_LOG(FLUX_LOG_ERR,
				 "attach shared rx shm failed key=%u len=%zu: %s\n",
				 ack.rx_shm_key, flux_iok_client.rx_len,
				 strerror(errno));
			goto fail_map;
		}
		rx_mapped = true;
	} else {
		flux_iok_client.rx_len = 0;
	}
#endif

	ret = flux_iok_client_init_queues(flux_iok_client.shm_base);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to initialize iok queues: %d\n",
			 ret);
		goto fail_map;
	}

	memset(flux_iok_client.timer_base, 0, flux_iok_client.timer_len);

	ret = flux_iok_client_register_shm(memfd);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to register iok shared memory: %d\n", ret);
		goto fail_map;
	}

	ret = flux_iok_dma_allocator_init();
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to initialize iok dma allocator: %d\n", ret);
		goto fail_map;
	}

	flux_iok_client.ready = true;
	close(memfd);
	return 0;

fail_map:
	free(flux_iok_client.pcpus);
	flux_iok_client.pcpus = NULL;
	if (rx_mapped && flux_iok_client.rx_base && flux_iok_client.rx_len)
		flux_mem_unmap_shm(flux_iok_client.rx_base);
	flux_iok_client.rx_base = NULL;
	flux_iok_client.rx_len = 0;
	munmap(flux_iok_client.shm_base, flux_iok_client.shm_len);
	flux_iok_client.shm_base = NULL;
	flux_iok_client.timer_base = NULL;
	flux_iok_client.timer_len = 0;
	flux_iok_client.net_enabled = false;
fail_memfd:
	close(memfd);
fail_fd:
	close(fd);
	flux_iok_client.fd = -1;
	return ret;
}

void flux_iok_client_fini(void)
{
	flux_iok_client.ready = false;
	if (flux_iok_client.fd >= 0)
		close(flux_iok_client.fd);
	flux_iok_client.fd = -1;
	if (flux_iok_client.rx_base && flux_iok_client.rx_len)
		flux_mem_unmap_shm(flux_iok_client.rx_base);
	flux_iok_client.rx_base = NULL;
	flux_iok_client.rx_len = 0;
	if (flux_iok_client.shm_base)
		munmap(flux_iok_client.shm_base, flux_iok_client.shm_len);
	flux_iok_client.shm_base = NULL;
	flux_iok_client.timer_base = NULL;
	flux_iok_client.timer_len = 0;
	flux_iok_client.net_enabled = false;
	free(flux_iok_client.pcpus);
	flux_iok_client.pcpus = NULL;
}

int flux_iok_client_fill_netdev(struct flux_fnet_netdev *dev)
{
	int i;

	if (!flux_iok_client.ready)
		return -FLUX_ENODEV;

	dev->nb_rx_queues = flux_env.nr_cpus;
	dev->nb_tx_queues = flux_env.nr_cpus;
	dev->mtu = flux_iok_client.mtu;
	dev->mode = FLUX_FNET_MODE_KERNEL;
	dev->csum_offload = flux_iok_client.csum_offload;
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
	dev->rx_base = flux_iok_client.rx_base;
	dev->rx_len = flux_iok_client.rx_len;
	memcpy(dev->eth_addr, flux_iok_client.host_mac, sizeof(dev->eth_addr));

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
		flux_iok_qspec_from_out(&flux_iok_client.pcpus[i].rxq,
					&dev->rxqs[i]);
		flux_iok_qspec_from_in(&flux_iok_client.pcpus[i].txpktq,
				       &dev->txpktqs[i]);
		flux_iok_qspec_from_in(&flux_iok_client.pcpus[i].txcmdq,
				       &dev->txcmdqs[i]);
	}

	return 0;
}

void *flux_iok_client_timer_alloc(int cpu, bool oneshot)
{
	struct flux_iok_timer_entry *timers = flux_iok_client.timer_base;

	if (!flux_iok_client.ready || !timers || cpu < 0 ||
	    cpu >= flux_env.nr_cpus || !oneshot)
		return NULL;

	return &timers[cpu];
}

int flux_iok_client_timer_set_oneshot(void *timer, unsigned long ns)
{
	struct flux_iok_timer_entry *entry = timer;
	uint64_t deadline;

	if (!entry || !ns)
		return -FLUX_EINVAL;

	deadline = now_ns() + ns;
	atomic_store_release(&entry->deadline_ns, deadline);
	return 0;
}
