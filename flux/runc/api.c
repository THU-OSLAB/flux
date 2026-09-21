#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <flux.h>

#include "runc.h"

static char flux_runc_error[512];

void flux_runc_set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(flux_runc_error, sizeof(flux_runc_error), fmt, ap);
	va_end(ap);
}

const char *flux_runc_last_error(void)
{
	return flux_runc_error[0] ? flux_runc_error :
		       "flux-runc command failed";
}

void flux_runc_log_errno(const char *what, int err)
{
	flux_runc_set_error("%s: %s", what, strerror(-err));
	FLUX_LOG(FLUX_LOG_ERR, "%s: %s\n", what, strerror(-err));
}

int flux_runc_state_set_pid_file(struct flux_runc_state *state,
				 const char *pid_file)
{
	char cwd[PATH_MAX];
	char *copy;
	int len;

	if (!pid_file || !pid_file[0]) {
		free(state->pid_file_path);
		state->pid_file_path = NULL;
		return 0;
	}

	if (pid_file[0] == '/') {
		copy = strdup(pid_file);
	} else {
		if (!getcwd(cwd, sizeof(cwd)))
			return -errno;

		len = snprintf(NULL, 0, "%s/%s", cwd, pid_file);
		if (len < 0)
			return -EINVAL;

		copy = malloc((size_t)len + 1);
		if (copy)
			snprintf(copy, (size_t)len + 1, "%s/%s", cwd, pid_file);
	}
	if (!copy)
		return -ENOMEM;

	free(state->pid_file_path);
	state->pid_file_path = copy;
	return 0;
}

int flux_runc_create_state(const char *bundle_dir, const char *id,
			   const char *pid_file, struct flux_runc_state *state)
{
	const struct flux_oci_cfg *oci;
	const char *run_cfg_path;
	int ret;

	ret = flux_launch_ensure_run_cfg_path();
	if (ret < 0) {
		flux_runc_set_error("failed to resolve default run cfg path");
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to resolve default run cfg path\n");
		return ret;
	}

	run_cfg_path = getenv("FLUX_RUN_CFG_FILE");
	if (!run_cfg_path || !run_cfg_path[0])
	{
		flux_runc_set_error("FLUX_RUN_CFG_FILE is not set");
		return -EINVAL;
	}

	if (flux_runc_set_bundle(bundle_dir) < 0) {
		flux_runc_set_error("failed to resolve container bundle %s",
				    bundle_dir);
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to resolve container bundle %s\n", bundle_dir);
		return -EINVAL;
	}

	ret = flux_runc_load_bundle();
	if (ret < 0) {
		flux_runc_set_error("failed to load or validate OCI config in %s",
				    bundle_dir);
		FLUX_LOG(FLUX_LOG_ERR, "failed to load container bundle\n");
		goto out_unload;
	}

	oci = flux_oci_cfg_get();
	if (!oci || !oci->bundle_dir) {
		flux_runc_set_error("OCI bundle state is unavailable");
		ret = -EINVAL;
		goto out_unload;
	}

	ret = flux_runc_state_prepare_new(state, id, oci->bundle_dir,
					  oci->terminal);
	if (ret < 0) {
		flux_runc_log_errno("failed to create runtime state", ret);
		goto out_unload;
	}

	ret = flux_runc_state_set_pid_file(state, pid_file);
	if (ret < 0) {
		flux_runc_log_errno("failed to persist pid file path", ret);
		goto out_remove;
	}

	ret = flux_runc_state_set_cgroup_path(state, oci->cgroups_path);
	if (ret < 0) {
		flux_runc_log_errno("failed to persist cgroup path", ret);
		goto out_remove;
	}
	ret = flux_runc_state_save(state);
	if (ret < 0) {
		flux_runc_log_errno("failed to persist cgroup ownership", ret);
		goto out_remove;
	}
	ret = flux_runc_cgroup_create(state);
	if (ret < 0) {
		flux_runc_log_errno("failed to create container cgroup", ret);
		goto out_remove;
	}
	ret = flux_runc_cgroup_apply(state, &oci->resources);
	if (ret < 0) {
		flux_runc_log_errno("failed to apply container resources", ret);
		goto out_remove;
	}

	ret = flux_runc_state_snapshot_bundle_config(state);
	if (ret < 0) {
		flux_runc_log_errno("failed to snapshot OCI config", ret);
		goto out_remove;
	}

	ret = flux_runc_state_snapshot_run_config(state, run_cfg_path);
	if (ret < 0) {
		flux_runc_log_errno("failed to snapshot run cfg", ret);
		goto out_remove;
	}
	ret = flux_runc_resources_apply_run_config(state, &oci->resources);
	if (ret < 0) {
		flux_runc_log_errno("failed to translate Flux resources", ret);
		goto out_remove;
	}
	ret = flux_runc_state_save_resources(state, &oci->resources);
	if (ret < 0) {
		flux_runc_log_errno("failed to persist container resources", ret);
		goto out_remove;
	}

	state->status = FLUX_RUNC_CREATED;
	ret = flux_runc_state_save(state);
	if (ret < 0) {
		flux_runc_log_errno("failed to persist runtime state", ret);
		goto out_remove;
	}

	flux_runc_unload();
	return 0;

out_remove:
	(void)flux_runc_state_cleanup_artifacts(state);
	(void)flux_runc_state_remove(state);
	flux_runc_state_fini(state);
out_unload:
	flux_runc_unload();
	return ret;
}

int flux_runc_create_loaded_state(struct flux_runc_state *state,
				  const char *console_socket)
{
	if (!state)
		return -EINVAL;

	if (state->terminal) {
		if (console_socket && console_socket[0])
			return flux_runc_runner_create_terminal_detached(
				state, state->pid_file_path, console_socket);
		return 0;
	}

	return flux_runc_runner_create_background(state, state->pid_file_path);
}

int flux_runc_reconcile_loaded_state(struct flux_runc_state *state,
				     const char *what)
{
	int ret;

	ret = flux_runc_state_reconcile(state);
	if (ret < 0)
		flux_runc_log_errno(what, ret);
	return ret;
}

int flux_runc_start_loaded_state(struct flux_runc_state *state)
{
	if (state->init_pid > 0)
		return flux_runc_runner_continue(state);

	if (state->terminal)
		return -EOPNOTSUPP;

	return flux_runc_runner_start_background(state, state->pid_file_path);
}

int flux_runc_run_loaded_state(struct flux_runc_state *state, bool detach)
{
	int exit_code = EXIT_FAILURE;
	int ret;

	if (detach) {
		if (state->terminal)
			return -EOPNOTSUPP;

		return flux_runc_runner_start_background(state,
							 state->pid_file_path);
	}

	ret = flux_runc_runner_start_foreground(state, state->pid_file_path,
						&exit_code);
	if (ret < 0)
		return ret;

	return exit_code;
}

static size_t flux_runc_exec_ring_len(void)
{
	return align_up(sizeof(struct flux_exec_ring), (size_t)CACHE_LINE_SIZE);
}

static void flux_runc_exec_ring_reset(struct flux_exec_ring *ring)
{
	unsigned int i;

	if (!ring)
		return;

	memset(ring, 0, sizeof(*ring));
	ring->hdr.size = FLUX_EXEC_RING_SLOTS;
	ring->hdr.init_status = -1;
	ring->hdr.next_seq = 1;
	for (i = 0; i < FLUX_EXEC_RING_SLOTS; i++)
		ring->slots[i].state = FLUX_EXEC_SLOT_FREE;
}

static int flux_runc_exec_ring_prepare_fd(const char *name, bool create,
					  int *fd_out)
{
	size_t len;
	int oflags;
	int fd;

	if (!name || !name[0] || !fd_out)
		return -EINVAL;

	len = flux_runc_exec_ring_len();
	oflags = O_RDWR;
	if (create)
		oflags |= O_CREAT;

	fd = shm_open(name, oflags, 0600);
	if (fd < 0)
		return -errno;

	if (create && ftruncate(fd, (off_t)len) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	*fd_out = fd;
	return 0;
}

int flux_runc_exec_ring_open(const char *name, bool create, bool fixed_addr,
			     struct flux_exec_ring **ring_out, int *fd_out)
{
	struct flux_exec_ring *ring;
	int fd = -1;
	void *addr;
	int ret;

	if (!name || !name[0] || !ring_out)
		return -EINVAL;

	ret = flux_runc_exec_ring_prepare_fd(name, create, &fd);
	if (ret < 0)
		return ret;

	addr = mmap(fixed_addr ? (void *)FLUX_EXEC_RING_MAP_ADDR : NULL,
		    flux_runc_exec_ring_len(), PROT_READ | PROT_WRITE,
#ifdef MAP_FIXED_NOREPLACE
		    MAP_SHARED | (fixed_addr ? MAP_FIXED_NOREPLACE : 0),
#else
		    MAP_SHARED | (fixed_addr ? MAP_FIXED : 0),
#endif
		    fd, 0);
	if (addr == MAP_FAILED) {
		int err = -errno;

		close(fd);
		return err;
	}

	ring = addr;
	if (create)
		flux_runc_exec_ring_reset(ring);

	*ring_out = ring;
	if (fd_out)
		*fd_out = fd;
	else
		close(fd);
	return 0;
}

int flux_runc_exec_ring_create(const char *name)
{
	struct flux_exec_ring ring;
	int fd = -1;
	ssize_t nw;
	int ret;

	if (!name || !name[0])
		return -EINVAL;

	ret = flux_runc_exec_ring_prepare_fd(name, true, &fd);
	if (ret < 0)
		return ret;

	flux_runc_exec_ring_reset(&ring);

	nw = pwrite(fd, &ring, sizeof(ring), 0);
	if (nw != (ssize_t)sizeof(ring)) {
		int err = nw < 0 ? -errno : -EIO;

		close(fd);
		return err;
	}

	close(fd);
	return 0;
}

void flux_runc_exec_ring_close(struct flux_exec_ring *ring, int fd)
{
	if (ring && ring != MAP_FAILED)
		munmap(ring, flux_runc_exec_ring_len());
	if (fd >= 0)
		close(fd);
}
