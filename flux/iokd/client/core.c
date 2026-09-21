#define _GNU_SOURCE
#define FLUX_FMT "iokd-client: "

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
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

#include "internal.h"

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif

uint32_t flux_iokd_client_tx_quantum(const struct flux_iokd_client *client)
{
	uint64_t shares;
	uint64_t quantum;

	if (!client)
		return 1;
	shares = atomic_load_relaxed(&client->cpu_shares);
	if (shares == 0)
		shares = 1024;
	if (shares < 2)
		shares = 2;
	if (shares > 262144)
		shares = 262144;
	quantum = (shares * 8 + 1023) / 1024;
	if (quantum < 1)
		quantum = 1;
	if (quantum > FLUX_FNET_TX_BURST_SIZE)
		quantum = FLUX_FNET_TX_BURST_SIZE;
	return (uint32_t)quantum;
}

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

	for (cpu = 0; cpu < flux_iokd.nr_host_cpus && cpu < CPU_SETSIZE;
	     cpu++) {
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

	flux_iokd.cpu_busy =
		calloc((size_t)flux_shm->nr_cpus, sizeof(*flux_iokd.cpu_busy));
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

static int flux_iokd_assign_client_cpus(
	struct flux_iokd_client *client, const struct flux_iok_ctrl_attach *attach)
{
	int assigned[CONFIG_FLUX_MAX_CPUS];
	uint32_t requested_cpus;
	unsigned int nr = 0;
	int cpu;

	if (!client || !attach)
		return -FLUX_EINVAL;
	requested_cpus = attach->requested_cpus;
	if (requested_cpus == 0 ||
	    requested_cpus > CONFIG_FLUX_MAX_CPUS || !flux_iokd.cpu_busy ||
	    !flux_iokd.cpu_allowed)
		return -FLUX_EINVAL;

	pthread_mutex_lock(&flux_iokd.cpu_lock);
	if (attach->flags & FLUX_IOK_ATTACH_F_CPU_LIST) {
		for (nr = 0; nr < requested_cpus; nr++) {
			unsigned int prev;

			cpu = attach->cpu_list[nr];
			if (cpu < 0 || cpu >= flux_iokd.nr_host_cpus ||
			    !flux_iokd.cpu_allowed[cpu] || cpu == flux_iokd_cfg.cpu ||
			    cpu == flux_iokd.control_cpu || flux_iokd.cpu_busy[cpu])
				goto no_space;
			for (prev = 0; prev < nr; prev++)
				if (assigned[prev] == cpu)
					goto invalid;
			assigned[nr] = cpu;
		}
	} else {
		for (cpu = 0; cpu < flux_iokd.nr_host_cpus; cpu++) {
			if (!flux_iokd.cpu_allowed[cpu] ||
			    cpu == flux_iokd_cfg.cpu ||
			    cpu == flux_iokd.control_cpu ||
			    flux_iokd.cpu_busy[cpu])
				continue;

			assigned[nr++] = cpu;
			if (nr == requested_cpus)
				break;
		}
	}

	if (nr != requested_cpus) {
		goto no_space;
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
no_space:
	pthread_mutex_unlock(&flux_iokd.cpu_lock);
	return -FLUX_ENOSPC;
invalid:
	pthread_mutex_unlock(&flux_iokd.cpu_lock);
	return -FLUX_EINVAL;
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
	while (refcnt != 0) {
		if (!__atomic_compare_exchange_n(
			    &client->refcnt, &refcnt, refcnt - 1, false,
			    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
			continue;

		if (refcnt == 1)
			flux_iokd_control_wake();
		break;
	}
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
	if (client->control_thread_started) {
		pthread_join(client->control_thread, NULL);
		client->control_thread_started = false;
	}

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
	flux_iokd_mlx5_client_destroy(client);

	if (client->fd >= 0)
		close(client->fd);

	flux_iokd_rx_mbuf_reclaim_all(client);

	for (m = client->dma_maps; m; m = tmp) {
		tmp = m->next;
		dma_maps++;
#ifdef CONFIG_FLUX_FNET
		if (!flux_iokd_cfg.mlx5_external && !flux_iokd_cfg.tx_copy)
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
	if (!flux_iokd_cfg.mlx5_external)
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
		if (flux_iokd_cfg.mlx5_external &&
		    !client->mlx5_clean_shutdown) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"client=%d mlx5 control channel lost; terminating client before teardown\n",
				client->id);
			flux_iokd_request_client_exit(client->peer_pid,
						      SIGKILL);
		}
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
	if (!flux_iokd_cfg.mlx5_external) {
		ret = rte_thread_register();
		if (ret < 0) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"failed to register control worker with DPDK: %s\n",
				rte_strerror(rte_errno));
			flux_iokd_request_shutdown();
			return NULL;
		}
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
		flux_iokd_reap_retired();
		pthread_mutex_lock(&flux_iokd.control_lock);
	}
	pthread_mutex_unlock(&flux_iokd.control_lock);
#ifdef CONFIG_FLUX_FNET
	if (!flux_iokd_cfg.mlx5_external)
		rte_thread_unregister();
#endif
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

static int flux_iokd_parent_pid(pid_t pid, pid_t *parent)
{
	char path[64];
	char line[256];
	FILE *file;

	if (snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid) >=
	    (int)sizeof(path))
		return -FLUX_EINVAL;
	file = fopen(path, "r");
	if (!file)
		return -errno;
	while (fgets(line, sizeof(line), file)) {
		long value;
		if (sscanf(line, "PPid:%ld", &value) == 1) {
			fclose(file);
			*parent = (pid_t)value;
			return 0;
		}
	}
	fclose(file);
	return -FLUX_ENOENT;
}

static bool flux_iokd_pid_matches(pid_t actual, pid_t requested)
{
	int depth;

	for (depth = 0; depth < 8 && actual > 1; depth++) {
		pid_t parent = 0;
		if (actual == requested)
			return true;
		if (flux_iokd_parent_pid(actual, &parent) < 0 || parent == actual)
			break;
		actual = parent;
	}
	return false;
}

static struct flux_iokd_client *
flux_iokd_find_client_by_pid(pid_t peer_pid, uid_t peer_uid)
{
	int i;

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		struct flux_iokd_client *client = flux_iokd_client_load(i);

		if (!client || !flux_iokd_pid_matches(client->peer_pid, peer_pid) ||
		    client->peer_uid != peer_uid ||
		    !flux_iokd_client_get(client))
			continue;
		return client;
	}
	return NULL;
}

static int flux_iokd_resource_request(
	int fd, const struct flux_iok_ctrl_hdr *hdr)
{
	struct flux_iok_ctrl_resource_request request = { .hdr = *hdr };
	struct flux_iok_ctrl_resource_reply reply = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = FLUX_IOK_CTRL_RESOURCE_REPLY,
			.len = sizeof(reply),
		},
		.status = -FLUX_ENOENT,
		.client_id = -1,
	};
	struct flux_iokd_client *client = NULL;
	struct ucred cred;
	socklen_t cred_len = sizeof(cred);
	int ret;

	if (hdr->len != sizeof(request))
		return -FLUX_EPROTO;
	ret = flux_iokd_socket_recv(
		fd, (char *)&request + sizeof(request.hdr),
		sizeof(request) - sizeof(request.hdr), NULL);
	if (ret < 0)
		return ret;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0)
		return -errno;
	client = flux_iokd_find_client_by_pid((pid_t)request.peer_pid,
					      cred.uid);
	if (!client)
		goto reply;

	if (hdr->op == FLUX_IOK_CTRL_RESOURCE_UPDATE) {
		atomic_store_relaxed(&client->io_weight, request.io_weight);
		atomic_store_relaxed(&client->net_class_id,
				     request.net_class_id);
		atomic_store_relaxed(&client->net_priority,
				     request.net_priority);
		atomic_store_relaxed(&client->cpu_shares, request.cpu_shares);
		atomic_store_relaxed(&client->cpu_quota, request.cpu_quota);
		atomic_store_relaxed(&client->cpu_period, request.cpu_period);
	}
	reply.status = 0;
	reply.client_id = client->id;
	reply.io_weight = atomic_load_relaxed(&client->io_weight);
	reply.net_class_id = atomic_load_relaxed(&client->net_class_id);
	reply.net_priority = atomic_load_relaxed(&client->net_priority);
	reply.cpu_shares = atomic_load_relaxed(&client->cpu_shares);
	reply.cpu_quota = atomic_load_relaxed(&client->cpu_quota);
	reply.cpu_period = atomic_load_relaxed(&client->cpu_period);
	reply.nr_cpus = (uint32_t)client->nr_cpus;
	for (int i = 0; i < client->nr_cpus; i++)
		reply.cpu_list[i] = client->cpu_list[i];
	reply.rx_packets = atomic_load_relaxed(&client->rx_packets);
	reply.rx_bytes = atomic_load_relaxed(&client->rx_bytes);
	reply.tx_packets = atomic_load_relaxed(&client->tx_packets);
	reply.tx_bytes = atomic_load_relaxed(&client->tx_bytes);
reply:
	ret = flux_iokd_socket_send(fd, &reply, sizeof(reply), -1);
	if (client)
		flux_iokd_client_put(client);
	return ret;
}

static void *flux_iokd_accept_thread(void *arg)
{
	int bind_ret;

	(void)arg;
	bind_ret = flux_iokd_bind_control_cpu();
	if (bind_ret < 0)
		FLUX_LOG(FLUX_LOG_WARN,
			 "failed to bind accept thread to cpu=%d: %s\n",
			 flux_iokd.control_cpu, strerror(-bind_ret));

	for (;;) {
		struct flux_iok_ctrl_hdr hdr;
		struct flux_iok_ctrl_attach attach;
		struct flux_iok_ctrl_ack ack;
		struct flux_iok_ctrl_register reg;
		struct flux_iok_ctrl_mlx5_prepare mlx5_prepare;
		struct flux_iokd_client *client = NULL;
		int mlx5_fds[2] = { -1, -1 };
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

		if (flux_iokd_socket_recv(fd, &hdr, sizeof(hdr), NULL) < 0)
			goto fail_fd;
		if (hdr.magic != FLUX_IOK_CTRL_MAGIC ||
		    hdr.version != FLUX_IOK_CTRL_VERSION)
			goto fail_fd;
		if (hdr.op == FLUX_IOK_CTRL_RESOURCE_UPDATE ||
		    hdr.op == FLUX_IOK_CTRL_RESOURCE_STATS) {
			(void)flux_iokd_resource_request(fd, &hdr);
			close(fd);
			continue;
		}
		if (hdr.op != FLUX_IOK_CTRL_ATTACH || hdr.len != sizeof(attach))
			goto fail_fd;
		memset(&attach, 0, sizeof(attach));
		attach.hdr = hdr;
		if (flux_iokd_socket_recv(
			    fd, (char *)&attach + sizeof(attach.hdr),
			    sizeof(attach) - sizeof(attach.hdr), NULL) < 0 ||
		    (attach.flags & ~FLUX_IOK_ATTACH_F_CPU_LIST))
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
		client->io_weight = attach.io_weight;
		client->net_class_id = attach.net_class_id;
		client->net_priority = attach.net_priority;
		client->cpu_shares = attach.cpu_shares;
		client->cpu_quota = attach.cpu_quota;
		client->cpu_period = attach.cpu_period;
		{
			struct ucred cred;
			socklen_t cred_len = sizeof(cred);

			if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred,
				       &cred_len) == 0) {
				client->peer_pid = cred.pid;
				client->peer_uid = cred.uid;
			} else {
				FLUX_LOG(FLUX_LOG_WARN,
					 "SO_PEERCRED failed fd=%d: %s\n", fd,
					 strerror(errno));
			}
		}
		if (attach.ip_addr && !flux_iokd.fnet_has_port) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"rejecting networked client: no network dataplane is available\n");
			goto fail_client;
		}
		if (flux_iokd_assign_client_cpus(client, &attach) < 0)
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

		memset(&ack, 0, sizeof(ack));
		ack.hdr.magic = FLUX_IOK_CTRL_MAGIC;
		ack.hdr.version = FLUX_IOK_CTRL_VERSION;
		ack.hdr.op = FLUX_IOK_CTRL_ACK;
		ack.hdr.len = sizeof(ack);
		ack.client_id = id;
		ack.mtu = flux_iokd_cfg.mtu;
		ack.csum_offload = flux_iokd.tx_chksum_offload;
		ack.nr_cpus = (uint32_t)client->nr_cpus;
		ack.window_base = client->window_base;
		ack.backend = flux_iokd_cfg.mlx5_external ?
				      FLUX_IOK_NET_BACKEND_MLX5_EXTERNAL :
				      FLUX_IOK_NET_BACKEND_LRPC;
		memcpy(ack.host_mac, flux_iokd.ctrl.host_mac,
		       sizeof(ack.host_mac));
		ack.rx_shm_key = flux_iokd.ctrl.rx_shbuf_key;
		ack.rx_len = flux_iokd.ctrl.rx_shbuf_len;
		ack.rx_pgsize = flux_iokd.ctrl.rx_shbuf_pgsize;
		for (i = 0; i < client->nr_cpus; i++)
			ack.cpu_list[i] = client->cpu_list[i];
		if (flux_iokd_socket_send(fd, &ack, sizeof(ack), -1) < 0)
			goto fail_client;

		if (flux_iokd_socket_recv(fd, &reg, sizeof(reg), &shm_fd) < 0)
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
		shm_fd = -1;
		if (client->shm_base == MAP_FAILED) {
			client->shm_base = NULL;
			goto fail_client;
		}

		client->timer_base = (void *)(uintptr_t)reg.timer_base;
		client->timer_len = reg.timer_len;
		client->rx_base = (void *)(uintptr_t)reg.rx_base;
		client->rx_len = reg.rx_len;
#ifdef CONFIG_FLUX_FNET
		if (client->ip_addr != 0 && !flux_iokd_cfg.mlx5_external) {
			if (!client->rx_base || client->rx_len != ack.rx_len ||
			    ack.rx_pgsize != PGSIZE_2MB)
				goto fail_client;
			client->tx_comp_max_ofs =
				FLUX_FNET_LRPC_QUEUE_SIZE * client->nr_cpus;
			client->tx_comp_ofq = calloc(client->tx_comp_max_ofs,
						     sizeof(unsigned long));
			if (!client->tx_comp_ofq)
				goto fail_client;
		}
#endif

		if (!flux_iokd_cfg.mlx5_external)
			flux_iokd_init_client_queues(client, &reg);
		if (client->ip_addr != 0 && flux_iokd_cfg.mlx5_external) {
			if (flux_iokd_mlx5_client_prepare(client, &mlx5_prepare,
							  mlx5_fds) < 0)
				goto fail_client;
			if (flux_iokd_socket_send_fds(fd, &mlx5_prepare,
						      sizeof(mlx5_prepare),
						      mlx5_fds, 2) < 0)
				goto fail_client;
			close(mlx5_fds[0]);
			close(mlx5_fds[1]);
			mlx5_fds[0] = mlx5_fds[1] = -1;
		}
		flux_iokd_client_set_ready(client, true);
		flux_iokd_client_store(client->id, client);
		if (!flux_iokd_cfg.mlx5_external)
			flux_iokd_rx_client_queue_register(client);
		if (pthread_create(&client->control_thread, NULL,
				   flux_iokd_client_control, client) != 0) {
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
		if (mlx5_fds[0] >= 0)
			close(mlx5_fds[0]);
		if (mlx5_fds[1] >= 0)
			close(mlx5_fds[1]);
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

	listen_fd =
		__atomic_exchange_n(&flux_iokd.listen_fd, -1, __ATOMIC_ACQ_REL);
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
