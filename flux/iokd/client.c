#define _GNU_SOURCE
#define FLUX_FMT "iokd-client: "

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef CONFIG_FLUX_FNET
#include <rte_errno.h>
#include <rte_lcore.h>
#endif

#include <kernel/asm/host_ops.h>
#include <kernel/linux/errno.h>
#include <utils/log.h>

#include "iokd.h"

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif

static const char *flux_iokd_skip_cpu_range_ws(const char *pos)
{
	while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
		pos++;

	return pos;
}

static int flux_iokd_parse_cpu_index(const char **pos_out, int *cpu_out)
{
	const char *pos = flux_iokd_skip_cpu_range_ws(*pos_out);
	char *end;
	long cpu;

	if (*pos < '0' || *pos > '9')
		return -FLUX_EINVAL;

	errno = 0;
	cpu = strtol(pos, &end, 10);
	if (errno || end == pos || cpu < 0 || cpu > INT_MAX)
		return -FLUX_EINVAL;

	*pos_out = end;
	*cpu_out = (int)cpu;
	return 0;
}

static int flux_iokd_parse_cpu_range(const char *spec, bool *allowed,
				     int nr_host_cpus, int *nr_allowed)
{
	const char *pos;
	int allowed_count = 0;
	bool expect_item = true;

	if (!spec || !allowed || nr_host_cpus <= 0)
		return -FLUX_EINVAL;

	memset(allowed, 0, sizeof(*allowed) * (size_t)nr_host_cpus);
	pos = spec;
	for (;;) {
		int start_cpu;
		int end_cpu;
		int cpu;
		int ret;

		pos = flux_iokd_skip_cpu_range_ws(pos);
		if (*pos == '\0')
			break;
		if (!expect_item)
			return -FLUX_EINVAL;

		ret = flux_iokd_parse_cpu_index(&pos, &start_cpu);
		if (ret < 0)
			return ret;
		end_cpu = start_cpu;

		pos = flux_iokd_skip_cpu_range_ws(pos);
		if (*pos == '-') {
			pos++;
			ret = flux_iokd_parse_cpu_index(&pos, &end_cpu);
			if (ret < 0 || end_cpu < start_cpu)
				return -FLUX_EINVAL;
		}

		if (start_cpu >= nr_host_cpus || end_cpu >= nr_host_cpus)
			return -FLUX_EINVAL;

		for (cpu = start_cpu; cpu <= end_cpu; cpu++) {
			if (allowed[cpu])
				continue;
			allowed[cpu] = true;
			allowed_count++;
		}
		expect_item = false;

		pos = flux_iokd_skip_cpu_range_ws(pos);
		if (*pos == '\0')
			break;
		if (*pos != ',')
			return -FLUX_EINVAL;
		pos++;
		expect_item = true;
	}

	if (expect_item || allowed_count == 0)
		return -FLUX_EINVAL;

	*nr_allowed = allowed_count;
	return 0;
}

static int flux_iokd_select_runtime_cpu(void)
{
	cpu_set_t affinity;
	int cpu;

	if (!flux_iokd.cpu_allowed || flux_iokd.nr_host_cpus <= 0)
		return -FLUX_EINVAL;

	if (sched_getaffinity(0, sizeof(affinity), &affinity) < 0)
		return -errno;

	for (cpu = 0; cpu < flux_iokd.nr_host_cpus && cpu < CPU_SETSIZE; cpu++) {
		if (!flux_iokd.cpu_allowed[cpu] || !CPU_ISSET(cpu, &affinity))
			continue;

		flux_iokd_cfg.cpu = cpu;
		return 0;
	}

	return -FLUX_EINVAL;
}

static int flux_iokd_select_control_cpu(void)
{
	int cpu;

	for (cpu = flux_iokd.nr_host_cpus - 1; cpu >= 0; cpu--) {
		if (!flux_iokd.cpu_allowed[cpu] || cpu == flux_iokd_cfg.cpu)
			continue;

		flux_iokd.control_cpu = cpu;
		return 0;
	}

	return -FLUX_ENOSPC;
}

static int flux_iokd_bind_control_cpu(void)
{
	cpu_set_t set;

	if (flux_iokd.control_cpu < 0 || flux_iokd.control_cpu >= CPU_SETSIZE)
		return -FLUX_EINVAL;

	CPU_ZERO(&set);
	CPU_SET(flux_iokd.control_cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		return -errno;

	return 0;
}

int flux_iokd_cpu_allocator_init(void)
{
	int ret;

	if (!flux_shm || flux_shm->nr_cpus <= 0)
		return -FLUX_EINVAL;

	flux_iokd.cpu_busy = calloc((size_t)flux_shm->nr_cpus,
				    sizeof(*flux_iokd.cpu_busy));
	if (!flux_iokd.cpu_busy)
		return -FLUX_ENOMEM;

	flux_iokd.cpu_allowed = calloc((size_t)flux_shm->nr_cpus,
				       sizeof(*flux_iokd.cpu_allowed));
	if (!flux_iokd.cpu_allowed) {
		free(flux_iokd.cpu_busy);
		flux_iokd.cpu_busy = NULL;
		return -FLUX_ENOMEM;
	}

	flux_iokd.nr_host_cpus = flux_shm->nr_cpus;
	ret = flux_iokd_parse_cpu_range(flux_iokd_cfg.cpu_range,
					flux_iokd.cpu_allowed,
					flux_iokd.nr_host_cpus,
					&flux_iokd.nr_allowed_cpus);
	if (ret < 0)
		goto fail;

	ret = flux_iokd_select_runtime_cpu();
	if (ret < 0)
		goto fail;
	ret = flux_iokd_select_control_cpu();
	if (ret < 0)
		goto fail;

	FLUX_LOG(FLUX_LOG_INFO,
		 "cpu_range=%s dataplane_cpu=%d control_cpu=%d allowed=%d\n",
		 flux_iokd_cfg.cpu_range, flux_iokd_cfg.cpu,
		 flux_iokd.control_cpu, flux_iokd.nr_allowed_cpus);
	return 0;

fail:
	free(flux_iokd.cpu_allowed);
	flux_iokd.cpu_allowed = NULL;
	free(flux_iokd.cpu_busy);
	flux_iokd.cpu_busy = NULL;
	flux_iokd.nr_host_cpus = 0;
	flux_iokd.nr_allowed_cpus = 0;
	flux_iokd.control_cpu = -1;
	return ret;
}

void flux_iokd_cpu_allocator_fini(void)
{
	free(flux_iokd.cpu_busy);
	flux_iokd.cpu_busy = NULL;
	free(flux_iokd.cpu_allowed);
	flux_iokd.cpu_allowed = NULL;
	flux_iokd.nr_host_cpus = 0;
	flux_iokd.nr_allowed_cpus = 0;
	flux_iokd.control_cpu = -1;
}

static int flux_iokd_assign_client_cpus(struct flux_iokd_client *client,
					uint32_t requested_cpus)
{
	int assigned[CONFIG_FLUX_MAX_CPUS];
	unsigned int nr = 0;
	int cpu;

	if (!client || requested_cpus == 0 ||
	    requested_cpus > CONFIG_FLUX_MAX_CPUS || !flux_iokd.cpu_busy ||
	    !flux_iokd.cpu_allowed)
		return -FLUX_EINVAL;

	pthread_mutex_lock(&flux_iokd.cpu_lock);
	for (cpu = 0; cpu < flux_iokd.nr_host_cpus; cpu++) {
		if (!flux_iokd.cpu_allowed[cpu] || cpu == flux_iokd_cfg.cpu ||
		    cpu == flux_iokd.control_cpu ||
		    flux_iokd.cpu_busy[cpu])
			continue;

		assigned[nr++] = cpu;
		if (nr == requested_cpus)
			break;
	}

	if (nr != requested_cpus) {
		pthread_mutex_unlock(&flux_iokd.cpu_lock);
		return -FLUX_ENOSPC;
	}

	for (nr = 0; nr < requested_cpus; nr++) {
		cpu = assigned[nr];
		flux_iokd.cpu_busy[cpu] = true;
		client->cpu_list[nr] = cpu;
	}
	pthread_mutex_unlock(&flux_iokd.cpu_lock);

	client->nr_cpus = (int)requested_cpus;
	client->cpu_assignment_active = true;
	return 0;
}

static void flux_iokd_release_client_cpus(struct flux_iokd_client *client)
{
	int i;

	if (!client || !client->cpu_assignment_active || !flux_iokd.cpu_busy)
		return;

	pthread_mutex_lock(&flux_iokd.cpu_lock);
	for (i = 0; i < client->nr_cpus; i++) {
		int cpu = client->cpu_list[i];

		if (cpu >= 0 && cpu < flux_iokd.nr_host_cpus)
			flux_iokd.cpu_busy[cpu] = false;
	}
	pthread_mutex_unlock(&flux_iokd.cpu_lock);

	client->cpu_assignment_active = false;
}

static int flux_iokd_client_id_alloc(void)
{
	unsigned int start;
	unsigned int i;

	start = flux_iokd.next_client_id % FLUX_IOKD_MAX_CLIENTS;
	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		unsigned int cand = (start + i) % FLUX_IOKD_MAX_CLIENTS;
		if (flux_iokd_client_load((int)cand))
			continue;
		flux_iokd.next_client_id = (cand + 1) % FLUX_IOKD_MAX_CLIENTS;
		return (int)cand;
	}

	return -1;
}

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

static const char *flux_iokd_format_ipv4(uint32_t ip, char *buf, size_t len)
{
	if (!ip) {
		snprintf(buf, len, "(none)");
		return buf;
	}

	snprintf(buf, len, "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
		 (ip >> 8) & 0xff, ip & 0xff);
	return buf;
}

static void flux_iokd_fixup_sock_perms(const char *path)
{
	struct stat st;
	const char *uid_s;
	const char *gid_s;
	uid_t uid;
	gid_t gid;
	char *end;

	uid_s = getenv("SUDO_UID");
	gid_s = getenv("SUDO_GID");
	if (uid_s && gid_s) {
		errno = 0;
		uid = (uid_t)strtoul(uid_s, &end, 10);
		if (errno || *end)
			uid_s = NULL;

		errno = 0;
		gid = (gid_t)strtoul(gid_s, &end, 10);
		if (errno || *end)
			gid_s = NULL;

		if (uid_s && gid_s && chown(path, uid, gid) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to chown %s to %u:%u: %s\n", path, uid,
				 gid, strerror(errno));
		}
	}

	if (chmod(path, 0666) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to chmod %s: %s\n", path,
			 strerror(errno));
		return;
	}

	if (stat(path, &st) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to stat %s: %s\n", path,
			 strerror(errno));
		return;
	}

	FLUX_LOG(FLUX_LOG_INFO, "socket %s ready uid=%u gid=%u mode=%o\n", path,
		 (unsigned int)st.st_uid, (unsigned int)st.st_gid,
		 st.st_mode & 0777);
}

static int flux_iokd_send_msg(int fd, const void *buf, size_t len, int send_fd)
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

static int flux_iokd_recv_msg(int fd, void *buf, size_t len, int *recv_fd)
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

bool flux_iokd_client_get(struct flux_iokd_client *client)
{
	unsigned int refcnt;

	if (!client)
		return false;

	refcnt = atomic_load_acquire(&client->refcnt);
	while (refcnt != 0) {
		if (__atomic_compare_exchange_n(
			    &client->refcnt, &refcnt, refcnt + 1, false,
			    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return true;
	}

	return false;
}

void flux_iokd_client_put(struct flux_iokd_client *client)
{
	unsigned int refcnt;

	if (!client)
		return;

	refcnt = atomic_load_relaxed(&client->refcnt);
	while (refcnt != 0 &&
	       !__atomic_compare_exchange_n(
		       &client->refcnt, &refcnt, refcnt - 1, false,
		       __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		;
}

void flux_iokd_client_destroy(struct flux_iokd_client *client)
{
	struct flux_iokd_dma_map *m;
	struct flux_iokd_dma_map *tmp;
	int id;
	size_t shm_len, rx_len, timer_len, dma_maps = 0;
	char ip_buf[INET_ADDRSTRLEN];

	if (!client)
		return;

	id = client->id;
	shm_len = client->shm_len;
	rx_len = client->rx_len;
	timer_len = client->timer_len;
	flux_iokd_format_ipv4(client->ip_addr, ip_buf, sizeof(ip_buf));

	if (id >= 0 && id < FLUX_IOKD_MAX_CLIENTS)
		atomic_store_relaxed(&flux_iokd.client_pids[id], 0);
	if (id >= 0 && id < FLUX_IOKD_MAX_CLIENTS &&
	    flux_iokd_client_load(id) == client)
		flux_iokd_client_store(id, NULL);

	FLUX_LOG(FLUX_LOG_INFO,
		 "destroy client=%d fd=%d refcnt=%u retired=%d ready=%d\n", id,
		 client->fd, atomic_load_relaxed(&client->refcnt),
		 client->retired, client->ready);

	flux_iokd_release_client_cpus(client);

	if (client->fd >= 0)
		close(client->fd);

	flux_iokd_rx_mbuf_reclaim_all(client);

	for (m = client->dma_maps; m; m = tmp) {
		tmp = m->next;
		dma_maps++;
#ifdef CONFIG_FLUX_FNET
		if (!flux_iokd_cfg.tx_copy)
			flux_iok_dma_unmap(&flux_iokd.ctrl,
					   (void *)m->iokd_addr, m->len,
					   m->pgsize, NULL);
#endif
		munmap((void *)m->iokd_addr, m->len);
		free(m);
	}

	if (client->shm_base && client->shm_base != MAP_FAILED)
		munmap(client->shm_base, client->shm_len);
	free(client->tx_comp_ofq);
	free(client);

	FLUX_LOG(FLUX_LOG_INFO, "client %d resources reclaimed\n", id);
	FLUX_LOG(FLUX_LOG_INFO,
		 "  ip=%s shm=%#zx rx=%#zx timer=%#zx dma_maps=%#zx\n", ip_buf,
		 shm_len, rx_len, timer_len, dma_maps);
}

static void flux_iokd_client_queue_retired(struct flux_iokd_client *client)
{
	struct flux_iokd_client *head;

	do {
		head = atomic_load_relaxed(&flux_iokd.retired_clients);
		client->retired_next = head;
	} while (!atomic_cmpxchg_release_relaxed(&flux_iokd.retired_clients,
						 &head, client));
}

void flux_iokd_client_retire(struct flux_iokd_client *client)
{
	if (!client)
		return;
	if (client->retired)
		return;
	client->retired = true;
	FLUX_LOG(FLUX_LOG_INFO,
		 "retire client=%d fd=%d refcnt=%u ready=%d dma_maps=%p\n",
		 client->id, client->fd, atomic_load_relaxed(&client->refcnt),
		 client->ready, client->dma_maps);

	flux_iokd_client_set_ready(client, false);
	/* Dataplane readers can still hold this pointer. Keep its slot, CPU
	 * assignment, mappings, and RX buffers until shutdown quiesces the NIC. */
	if (client->id >= 0 && client->id < FLUX_IOKD_MAX_CLIENTS)
		atomic_store_relaxed(&flux_iokd.client_pids[client->id], 0);
	flux_iokd_rx_client_queue_unregister(client);

	if (client->fd >= 0) {
		shutdown(client->fd, SHUT_RDWR);
		close(client->fd);
		client->fd = -1;
	}

	flux_iokd_client_queue_retired(client);
	flux_iokd_client_put(client);
}

void flux_iokd_client_request_retire(struct flux_iokd_client *client)
{
	if (!client)
		return;

	atomic_store_release(&client->retire_requested, true);
	flux_iokd_control_wake();
}

void flux_iokd_process_retire_requests(void)
{
	int i;

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		struct flux_iokd_client *client = flux_iokd_client_load(i);

		if (!client)
			continue;
		if (!atomic_load_acquire(&client->retire_requested))
			continue;

		atomic_store_relaxed(&client->retire_requested, false);
		flux_iokd_client_retire(client);
	}
}

void flux_iokd_reap_retired(void)
{
	struct flux_iokd_client *client;
	struct flux_iokd_client *next;

	client = __atomic_exchange_n(&flux_iokd.retired_clients, NULL,
				     __ATOMIC_ACQUIRE);
	while (client) {
		next = client->retired_next;
		client->retired_next = NULL;
		if (atomic_load_acquire(&client->refcnt) == 0)
			flux_iokd_client_destroy(client);
		else
			flux_iokd_client_queue_retired(client);
		client = next;
	}
}

void flux_iokd_control_wake(void)
{
	pthread_mutex_lock(&flux_iokd.control_lock);
	flux_iokd.control_work_pending = true;
	pthread_cond_signal(&flux_iokd.control_cond);
	pthread_mutex_unlock(&flux_iokd.control_lock);
}

static void *flux_iokd_control_worker(void *arg)
{
	int ret;

	(void)arg;
	ret = flux_iokd_bind_control_cpu();
	if (ret < 0)
		FLUX_LOG(FLUX_LOG_WARN,
			 "failed to bind control worker to cpu=%d: %s\n",
			 flux_iokd.control_cpu, strerror(-ret));

#ifdef CONFIG_FLUX_FNET
	ret = rte_thread_register();
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to register control worker with DPDK: %s\n",
			 rte_strerror(rte_errno));
		flux_iokd_request_shutdown();
		return NULL;
	}
#endif

	pthread_mutex_lock(&flux_iokd.control_lock);
	for (;;) {
		while (!flux_iokd.control_work_pending &&
		       !flux_iokd.control_worker_stop)
			pthread_cond_wait(&flux_iokd.control_cond,
					  &flux_iokd.control_lock);

		if (flux_iokd.control_worker_stop &&
		    !flux_iokd.control_work_pending)
			break;

		flux_iokd.control_work_pending = false;
		pthread_mutex_unlock(&flux_iokd.control_lock);
		flux_iokd_process_retire_requests();
		flux_iokd_rx_process_pending_clients();
		pthread_mutex_lock(&flux_iokd.control_lock);
	}
	pthread_mutex_unlock(&flux_iokd.control_lock);
#ifdef CONFIG_FLUX_FNET
	rte_thread_unregister();
#endif
	return NULL;
}

static void *flux_iokd_client_thread(void *arg)
{
	struct flux_iokd_client *client = arg;

	for (;;) {
		struct flux_iok_ctrl_hdr hdr;
		struct flux_iok_ctrl_dma_map req;
		struct flux_iokd_dma_map *m;
		void *addr;
		int recv_fd = -1;
		int ret;

		ret = flux_iokd_recv_msg(client->fd, &hdr, sizeof(hdr),
					 &recv_fd);
		if (ret < 0)
			break;

		if (hdr.magic != FLUX_IOK_CTRL_MAGIC ||
		    hdr.version != FLUX_IOK_CTRL_VERSION)
			break;
		if (hdr.len != sizeof(req) ||
		    (hdr.op != FLUX_IOK_CTRL_DMA_MAP &&
		     hdr.op != FLUX_IOK_CTRL_DMA_UNMAP))
			break;

		memcpy(&req, &hdr, sizeof(hdr));
		ret = flux_iokd_recv_msg(client->fd, (char *)&req + sizeof(hdr),
					 sizeof(req) - sizeof(hdr), NULL);
		if (ret < 0)
			break;

		if (hdr.op == FLUX_IOK_CTRL_DMA_MAP) {
			void *map_addr;

			if (recv_fd < 0)
				break;

			map_addr = flux_iokd_dma_map_addr(client, req.addr,
							  req.len);
			addr = mmap(map_addr, req.len, PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED_NOREPLACE, recv_fd,
				    0);
			close(recv_fd);
			if (addr == MAP_FAILED)
				break;

			m = calloc(1, sizeof(*m));
			if (!m) {
				munmap(addr, req.len);
				break;
			}

			m->client_addr = req.addr;
			m->iokd_addr = (uintptr_t)addr;
			m->len = req.len;
			m->pgsize = req.pgsize;
#ifdef CONFIG_FLUX_FNET
			/*
			 * The client always registers the shared TX region
			 * with iokd. Only the zero-copy mode additionally
			 * installs the NIC DMA mapping.
			 */
			if (!flux_iokd_cfg.tx_copy &&
			    flux_iok_dma_map(&flux_iokd.ctrl, addr, req.len,
					     req.pgsize, NULL) < 0) {
				munmap(addr, req.len);
				free(m);
				break;
			}
#endif

			m->next = client->dma_maps;
			client->dma_maps = m;
			continue;
		}

		m = client->dma_maps;
		while (m) {
			if (m->client_addr == req.addr && m->len == req.len)
				break;
			m = m->next;
		}
		if (m) {
			struct flux_iokd_dma_map **pp = &client->dma_maps;
			while (*pp && *pp != m)
				pp = &(*pp)->next;
			if (*pp == m)
				*pp = m->next;
#ifdef CONFIG_FLUX_FNET
			if (!flux_iokd_cfg.tx_copy)
				flux_iok_dma_unmap(&flux_iokd.ctrl,
						   (void *)m->iokd_addr, m->len,
						   m->pgsize, NULL);
#endif
			munmap((void *)m->iokd_addr, m->len);
			free(m);
		}
	}

	FLUX_LOG(FLUX_LOG_WARN, "client=%d control thread exiting\n",
		 client->id);
	flux_iokd_client_request_retire(client);
	return NULL;
}

static int
flux_iokd_init_client_queues(struct flux_iokd_client *client,
			     const struct flux_iok_ctrl_register *reg)
{
	int i;

	for (i = 0; i < client->nr_cpus; i++) {
		lrpc_init_out(&client->pcpus[i].rxq,
			      (struct lrpc_msg *)reg->rxqs[i].tbl,
			      reg->rxqs[i].size, reg->rxqs[i].wb);
		lrpc_init_in(&client->pcpus[i].txpktq,
			     (struct lrpc_msg *)reg->txpktqs[i].tbl,
			     reg->txpktqs[i].size, reg->txpktqs[i].wb);
		lrpc_init_in(&client->pcpus[i].txcmdq,
			     (struct lrpc_msg *)reg->txcmdqs[i].tbl,
			     reg->txcmdqs[i].size, reg->txcmdqs[i].wb);
	}

	return 0;
}

static void *flux_iokd_accept_thread(void *arg)
{
	int bind_ret;

	(void)arg;
	bind_ret = flux_iokd_bind_control_cpu();
	if (bind_ret < 0)
		FLUX_LOG(FLUX_LOG_WARN, "failed to bind accept thread to cpu=%d: %s\n",
			 flux_iokd.control_cpu, strerror(-bind_ret));

	for (;;) {
		struct flux_iok_ctrl_attach attach;
		struct flux_iok_ctrl_ack ack;
		struct flux_iok_ctrl_register reg;
		struct flux_iokd_client *client = NULL;
		int fd;
		int shm_fd = -1;
		int id;
		int i;

		fd = accept4(flux_iokd.listen_fd, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0) {
			if (flux_iokd.listen_fd < 0)
				break;
			continue;
		}

		if (flux_iokd_recv_msg(fd, &attach, sizeof(attach), NULL) < 0)
			goto fail_fd;
		if (attach.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
		    attach.hdr.version != FLUX_IOK_CTRL_VERSION ||
		    attach.hdr.op != FLUX_IOK_CTRL_ATTACH)
			goto fail_fd;

		client = calloc(1, sizeof(*client));
		if (!client)
			goto fail_fd;

		client->fd = fd;
		client->id = -1;
		atomic_store_relaxed(&client->refcnt, 1);
		client->ip_addr = attach.ip_addr;
		client->netmask = attach.netmask;
		client->gateway = attach.gateway;
		{
			struct ucred cred;
			socklen_t cred_len = sizeof(cred);

			if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred,
				       &cred_len) == 0) {
				client->peer_pid = cred.pid;
			} else {
				FLUX_LOG(FLUX_LOG_WARN,
					 "SO_PEERCRED failed fd=%d: %s\n", fd,
					 strerror(errno));
			}
		}
		if (attach.ip_addr && !flux_iokd.fnet_has_port) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "rejecting networked client: no network dataplane is available\n");
			goto fail_client;
		}
		if (flux_iokd_assign_client_cpus(client, attach.requested_cpus) <
		    0)
			goto fail_client;

		id = flux_iokd_client_id_alloc();
		if (id < 0)
			goto fail_client;

		client->id = id;
		if (client->peer_pid > 0)
			atomic_store_relaxed(&flux_iokd.client_pids[id],
					     client->peer_pid);
		client->window_base =
			FLUX_IOK_CLIENT_WINDOW_BASE +
			(uint64_t)id * FLUX_IOK_CLIENT_WINDOW_SIZE;

		ack.hdr.magic = FLUX_IOK_CTRL_MAGIC;
		ack.hdr.version = FLUX_IOK_CTRL_VERSION;
		ack.hdr.op = FLUX_IOK_CTRL_ACK;
		ack.hdr.len = sizeof(ack);
		ack.client_id = id;
		ack.mtu = flux_iokd_cfg.mtu;
		ack.csum_offload = flux_iokd.tx_chksum_offload;
		ack.nr_cpus = (uint32_t)client->nr_cpus;
		ack.window_base = client->window_base;
		memcpy(ack.host_mac, flux_iokd.ctrl.host_mac,
		       sizeof(ack.host_mac));
		ack.rx_shm_key = flux_iokd.ctrl.rx_shbuf_key;
		ack.rx_len = flux_iokd.ctrl.rx_shbuf_len;
		ack.rx_pgsize = flux_iokd.ctrl.rx_shbuf_pgsize;
		for (i = 0; i < client->nr_cpus; i++)
			ack.cpu_list[i] = client->cpu_list[i];
		if (flux_iokd_send_msg(fd, &ack, sizeof(ack), -1) < 0)
			goto fail_client;

		if (flux_iokd_recv_msg(fd, &reg, sizeof(reg), &shm_fd) < 0)
			goto fail_client;
		if (reg.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
		    reg.hdr.version != FLUX_IOK_CTRL_VERSION ||
		    reg.hdr.op != FLUX_IOK_CTRL_REGISTER || shm_fd < 0)
			goto fail_client;

		client->shm_len = reg.shm_len;
		client->shm_base = mmap((void *)(uintptr_t)reg.shm_base,
					reg.shm_len, PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_FIXED_NOREPLACE,
					shm_fd, 0);
		close(shm_fd);
		if (client->shm_base == MAP_FAILED) {
			client->shm_base = NULL;
			goto fail_client;
		}

		client->timer_base = (void *)(uintptr_t)reg.timer_base;
		client->timer_len = reg.timer_len;
		client->rx_base = (void *)(uintptr_t)reg.rx_base;
		client->rx_len = reg.rx_len;
#ifdef CONFIG_FLUX_FNET
		if (client->ip_addr != 0) {
			if (!client->rx_base || client->rx_len != ack.rx_len ||
			    ack.rx_pgsize != PGSIZE_2MB)
				goto fail_client;
			client->tx_comp_max_ofs =
				FLUX_FNET_LRPC_QUEUE_SIZE * client->nr_cpus;
			client->tx_comp_ofq = calloc(
				client->tx_comp_max_ofs,
				sizeof(unsigned long));
			if (!client->tx_comp_ofq)
				goto fail_client;
		}
#endif

		flux_iokd_init_client_queues(client, &reg);
		flux_iokd_client_set_ready(client, true);
		flux_iokd_client_store(client->id, client);
		flux_iokd_rx_client_queue_register(client);
		if (pthread_create(&client->control_thread, NULL,
				   flux_iokd_client_thread, client) != 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to create client=%d control thread\n",
				 client->id);
			flux_iokd_client_request_retire(client);
			continue;
		}
		client->control_thread_started = true;
		{
			char ip_buf[INET_ADDRSTRLEN];

			FLUX_LOG(FLUX_LOG_INFO, "client %d registered\n",
				 client->id);
			FLUX_LOG(
				FLUX_LOG_INFO,
				"  ip=%s cpus=%d pid=%d window=%#lx shm=%#zx rx=%#zx timer=%#zx\n",
				flux_iokd_format_ipv4(client->ip_addr, ip_buf,
						      sizeof(ip_buf)),
				client->nr_cpus, (int)client->peer_pid,
				(unsigned long)client->window_base,
				client->shm_len, client->rx_len,
				client->timer_len);
		}
		continue;

fail_client:
		FLUX_LOG(
			FLUX_LOG_WARN,
			"client setup failed fd=%d client_id=%d shm_fd=%d errno=%d (%s)\n",
			fd, client ? client->id : -1, shm_fd, errno,
			strerror(errno));
		if (shm_fd >= 0)
			close(shm_fd);
		if (client)
			flux_iokd_client_destroy(client);
		else
			close(fd);
		continue;
fail_fd:
		FLUX_LOG(FLUX_LOG_WARN,
			 "invalid attach on fd=%d errno=%d (%s)\n", fd, errno,
			 strerror(errno));
		close(fd);
	}

	return NULL;
}

int flux_iokd_control_init(void)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	mode_t old_umask;
	int ret;

	flux_iokd.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (flux_iokd.listen_fd < 0)
		return -errno;

	strncpy(addr.sun_path, flux_iokd_cfg.sock_path,
		sizeof(addr.sun_path) - 1);
	unlink(addr.sun_path);

	old_umask = umask(0);
	ret = bind(flux_iokd.listen_fd, (struct sockaddr *)&addr, sizeof(addr));
	umask(old_umask);
	if (ret < 0) {
		close(flux_iokd.listen_fd);
		flux_iokd.listen_fd = -1;
		return -errno;
	}

	flux_iokd_fixup_sock_perms(addr.sun_path);

	if (listen(flux_iokd.listen_fd, FLUX_IOKD_CTRL_BACKLOG) < 0) {
		close(flux_iokd.listen_fd);
		flux_iokd.listen_fd = -1;
		return -errno;
	}
	FLUX_LOG(FLUX_LOG_INFO, "listening on %s\n", addr.sun_path);

	return 0;
}

int flux_iokd_control_start(void)
{
	int ret;

	pthread_mutex_lock(&flux_iokd.control_lock);
	flux_iokd.control_worker_stop = false;
	flux_iokd.control_work_pending = false;
	pthread_mutex_unlock(&flux_iokd.control_lock);

	ret = pthread_create(&flux_iokd.control_worker_thread, NULL,
			     flux_iokd_control_worker, NULL);
	if (ret != 0)
		return -ret;
	flux_iokd.control_worker_started = true;

	ret = pthread_create(&flux_iokd.accept_thread, NULL,
			     flux_iokd_accept_thread, NULL);
	if (ret != 0) {
		pthread_mutex_lock(&flux_iokd.control_lock);
		flux_iokd.control_worker_stop = true;
		pthread_cond_signal(&flux_iokd.control_cond);
		pthread_mutex_unlock(&flux_iokd.control_lock);
		pthread_join(flux_iokd.control_worker_thread, NULL);
		flux_iokd.control_worker_started = false;
		return -ret;
	}
	flux_iokd.accept_thread_started = true;

	FLUX_LOG(FLUX_LOG_INFO, "control threads using cpu=%d\n",
		 flux_iokd.control_cpu);
	return 0;
}

void flux_iokd_control_fini(void)
{
	int listen_fd;
	int i;

	listen_fd = __atomic_exchange_n(&flux_iokd.listen_fd, -1,
					 __ATOMIC_ACQ_REL);
	if (listen_fd >= 0) {
		shutdown(listen_fd, SHUT_RDWR);
		close(listen_fd);
	}

	if (flux_iokd.accept_thread_started) {
		pthread_join(flux_iokd.accept_thread, NULL);
		flux_iokd.accept_thread_started = false;
	}

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		struct flux_iokd_client *client = flux_iokd_client_load(i);

		if (client && client->fd >= 0)
			shutdown(client->fd, SHUT_RDWR);
	}
	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		struct flux_iokd_client *client = flux_iokd_client_load(i);

		if (!client || !client->control_thread_started)
			continue;
		pthread_join(client->control_thread, NULL);
		client->control_thread_started = false;
	}

	if (flux_iokd.control_worker_started) {
		pthread_mutex_lock(&flux_iokd.control_lock);
		flux_iokd.control_work_pending = true;
		flux_iokd.control_worker_stop = true;
		pthread_cond_signal(&flux_iokd.control_cond);
		pthread_mutex_unlock(&flux_iokd.control_lock);
		pthread_join(flux_iokd.control_worker_thread, NULL);
		flux_iokd.control_worker_started = false;
	}

	if (flux_iokd_cfg.sock_path)
		unlink(flux_iokd_cfg.sock_path);
}
