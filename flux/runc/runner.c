#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <flux.h>

#include "runc.h"

struct flux_runc_spawn_msg {
	pid_t pid;
	int err;
};

static int flux_runc_wait_pid(pid_t pid, int *exit_code);
static int flux_runc_wait_pid_stopped(pid_t pid);
static int flux_runc_exec_child_main(const struct flux_runc_state *state,
				     bool start_suspended, int notify_fd,
				     int console_master_fd,
				     int console_slave_fd);
static void flux_runc_exec_child_or_exit(const struct flux_runc_state *state,
					 bool start_suspended, int notify_fd,
					 int console_master_fd,
					 int console_slave_fd)
	__attribute__((noreturn));
static int flux_runc_background_monitor_main(struct flux_runc_state *state,
					     const char *pid_file,
					     bool start_suspended,
					     enum flux_runc_status live_status,
					     int notify_fd,
					     int console_master_fd,
					     int console_slave_fd);
static void flux_runc_background_monitor_or_exit(struct flux_runc_state *state,
						 const char *pid_file,
						 bool start_suspended,
						 enum flux_runc_status live_status,
						 int notify_fd,
						 int console_master_fd,
						 int console_slave_fd)
	__attribute__((noreturn));

static int flux_runc_runner_prepare_start(const struct flux_runc_state *state)
{
	if (!state || !state->exec_ring_name)
		return -EINVAL;

	return flux_runc_exec_ring_create(state->exec_ring_name);
}

int flux_runc_state_control_pid(const struct flux_runc_state *state,
				pid_t *pid_out)
{
	char path[PATH_MAX];
	char buf[128];
	char *cursor;
	char *end;
	pid_t current;
	pid_t next;
	long child;
	ssize_t nr;
	int fd;
	int depth;

	if (!state || !pid_out || state->init_pid <= 0)
		return -ESRCH;

	current = state->init_pid;
	for (depth = 0; depth < 8; depth++) {
		if (snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
			     (long)current, (long)current) >=
		    (int)sizeof(path))
			break;

		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			break;

		nr = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (nr <= 0)
			break;

		buf[nr] = '\0';
		cursor = buf;
		while (*cursor && isspace((unsigned char)*cursor))
			cursor++;
		if (!*cursor)
			break;

		errno = 0;
		child = strtol(cursor, &end, 10);
		if (errno || end == cursor || child <= 0)
			break;

		while (*end && isspace((unsigned char)*end))
			end++;
		if (*end != '\0')
			break;

		next = (pid_t)child;
		if (kill(next, 0) < 0 && errno != EPERM)
			break;
		if (next == current)
			break;

		current = next;
	}

	*pid_out = current;

	return 0;
}

static void flux_runc_reap_killed_child(pid_t pid, int *exit_code)
{
	if (pid <= 0)
		return;

	kill(pid, SIGKILL);
	(void)flux_runc_wait_pid(pid, exit_code);
}

static void flux_runc_pipe_send(int fd, const struct flux_runc_spawn_msg *msg)
{
	const char *buf = (const char *)msg;
	size_t left = sizeof(*msg);

	while (left > 0) {
		ssize_t nw = write(fd, buf, left);

		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return;
		}

		buf += nw;
		left -= (size_t)nw;
	}
}

static void flux_runc_close_console_pair(int *master_fd, int *slave_fd)
{
	if (master_fd && *master_fd >= 0) {
		close(*master_fd);
		*master_fd = -1;
	}

	if (slave_fd && *slave_fd >= 0) {
		close(*slave_fd);
		*slave_fd = -1;
	}
}

static int flux_runc_get_self_path(char *buf, size_t size)
{
	ssize_t len;

	len = readlink("/proc/self/exe", buf, size - 1);
	if (len < 0)
		return -errno;
	if ((size_t)len >= size - 1)
		return -ENAMETOOLONG;

	buf[len] = '\0';
	return 0;
}

static int flux_runc_read_proc_state(pid_t pid, char *state)
{
	char path[PATH_MAX];
	char buf[256];
	char *rparen;
	ssize_t nr;
	int fd;

	if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	nr = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (nr < 0)
		return -errno;
	if (nr == 0)
		return -EIO;

	buf[nr] = '\0';
	rparen = strrchr(buf, ')');
	if (!rparen || rparen[1] != ' ' || !rparen[2])
		return -EINVAL;

	*state = rparen[2];
	return 0;
}

static int flux_runc_wait_pid(pid_t pid, int *exit_code)
{
	int status;

	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		return -errno;
	}

	if (WIFEXITED(status)) {
		*exit_code = WEXITSTATUS(status);
		return 0;
	}

	if (WIFSIGNALED(status)) {
		*exit_code = 128 + WTERMSIG(status);
		return 0;
	}

	*exit_code = EXIT_FAILURE;
	return 0;
}

static int flux_runc_wait_pid_stopped(pid_t pid)
{
	unsigned int i;

	for (i = 0; i < 500; i++) {
		char proc_state;
		int ret;

		ret = flux_runc_read_proc_state(pid, &proc_state);
		if (ret < 0) {
			if (ret == -ENOENT || ret == -ESRCH)
				return -ECHILD;
			return ret;
		}

		if (proc_state == 'T' || proc_state == 't')
			return 0;
		if (proc_state == 'Z')
			return -ECHILD;

		if (nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 10000000 },
			      NULL) < 0 && errno != EINTR)
			return -errno;
	}

	return -ETIMEDOUT;
}

static int flux_runc_read_spawn_msg(int fd, struct flux_runc_spawn_msg *msg)
{
	char *buf;
	size_t left;

	if (fd < 0 || !msg)
		return -EINVAL;

	buf = (char *)msg;
	left = sizeof(*msg);
	while (left > 0) {
		ssize_t nr = read(fd, buf, left);

		if (nr < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (nr == 0)
			return -EIO;

		buf += nr;
		left -= (size_t)nr;
	}

	return 0;
}

static int flux_runc_write_pid_file(const char *path, pid_t pid)
{
	char buf[64];
	ssize_t len;
	ssize_t nw;
	int fd;

	if (!path || !path[0])
		return 0;

	len = snprintf(buf, sizeof(buf), "%ld", (long)pid);
	if (len <= 0 || len >= (ssize_t)sizeof(buf))
		return -EINVAL;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;

	do {
		nw = write(fd, buf, (size_t)len);
	} while (nw < 0 && errno == EINTR);

	if (nw != len) {
		int ret = nw < 0 ? -errno : -EIO;

		close(fd);
		return ret;
	}

	if (close(fd) < 0)
		return -errno;

	return 0;
}

static int flux_runc_set_signal_bridge_env(void)
{
	const char *bridged_signals;
	char *merged;
	int len;

	bridged_signals = getenv(FLUX_SIGNAL_BRIDGE_ENV);
	if (!bridged_signals || !bridged_signals[0]) {
		if (setenv(FLUX_SIGNAL_BRIDGE_ENV,
			   FLUX_DEFAULT_SIGNAL_BRIDGE, 1) < 0)
			return -errno;
		return 0;
	}

	if (strstr(bridged_signals, "SIGUSR1"))
		return 0;

	len = snprintf(NULL, 0, "%s,%s", bridged_signals, "SIGUSR1");
	if (len < 0)
		return -EINVAL;

	merged = malloc((size_t)len + 1);
	if (!merged)
		return -ENOMEM;

	snprintf(merged, (size_t)len + 1, "%s,%s", bridged_signals, "SIGUSR1");
	if (setenv(FLUX_SIGNAL_BRIDGE_ENV, merged, 1) < 0) {
		free(merged);
		return -errno;
	}

	free(merged);
	return 0;
}

static int flux_runc_prepare_child_exec(const struct flux_runc_state *state,
					char *self_path, size_t self_path_size)
{
	int ret;

	if (!state || !self_path || !self_path_size)
		return -EINVAL;

	if (state->run_config_path &&
	    setenv("FLUX_RUN_CFG_FILE", state->run_config_path, 1) < 0)
		return -errno;

	ret = flux_runc_set_signal_bridge_env();
	if (ret < 0)
		return ret;

	return flux_runc_get_self_path(self_path, self_path_size);
}

static int flux_runc_state_mark_live(struct flux_runc_state *state, pid_t pid,
				     enum flux_runc_status status,
				     const char *pid_file)
{
	int ret;

	state->init_pid = pid;
	state->status = status;
	state->exit_code = -1;

	ret = flux_runc_state_save(state);
	if (ret < 0)
		return ret;

	return flux_runc_write_pid_file(pid_file ?: state->pid_file_path, pid);
}

static void flux_runc_state_mark_stopped(struct flux_runc_state *state,
					 int exit_code)
{
	state->status = FLUX_RUNC_STOPPED;
	state->exit_code = exit_code;
	flux_runc_state_save(state);
}

static int flux_runc_fork_exec_child(const struct flux_runc_state *state,
				     bool start_suspended, int notify_fd,
				     int console_master_fd,
				     int console_slave_fd,
				     pid_t *pid_out)
{
	pid_t pid;

	if (!state || !pid_out)
		return -EINVAL;

	pid = fork();
	if (pid < 0)
		return -errno;

	if (pid == 0)
		flux_runc_exec_child_or_exit(state, start_suspended, notify_fd,
					     console_master_fd,
					     console_slave_fd);

	*pid_out = pid;
	return 0;
}

static int flux_runc_exec_child_main(const struct flux_runc_state *state,
				     bool start_suspended, int notify_fd,
				     int console_master_fd,
				     int console_slave_fd)
{
	char self_path[PATH_MAX];
	char *const argv[] = { self_path, "__exec", state->id, NULL };
	int ret;

	if (notify_fd >= 0)
		close(notify_fd);

	if (console_slave_fd >= 0)
		flux_runc_console_setup_detached_terminal(console_master_fd,
							  console_slave_fd);
	else
		flux_runc_close_console_pair(&console_master_fd,
					     &console_slave_fd);

	ret = flux_runc_prepare_child_exec(state, self_path, sizeof(self_path));
	if (ret < 0)
		return ret;

	if (start_suspended)
		raise(SIGSTOP);

	execv(self_path, argv);
	return -errno;
}

static void flux_runc_exec_child_or_exit(const struct flux_runc_state *state,
					 bool start_suspended, int notify_fd,
					 int console_master_fd,
					 int console_slave_fd)
{
	int ret = flux_runc_exec_child_main(state, start_suspended, notify_fd,
					    console_master_fd,
					    console_slave_fd);

	_exit(ret < 0 ? 127 : ret);
}

static void flux_runc_send_spawn_reply(int notify_fd, pid_t pid, int err)
{
	struct flux_runc_spawn_msg msg = {
		.pid = pid,
		.err = err,
	};

	if (notify_fd >= 0)
		flux_runc_pipe_send(notify_fd, &msg);
}

static int flux_runc_background_monitor_main(struct flux_runc_state *state,
					     const char *pid_file,
					     bool start_suspended,
					     enum flux_runc_status live_status,
					     int notify_fd,
					     int console_master_fd,
					     int console_slave_fd)
{
	pid_t child = -1;
	int exit_code = EXIT_FAILURE;
	int ret;

	if (setsid() < 0) {
		ret = -errno;
		goto out_fail;
	}

	ret = flux_runc_fork_exec_child(state, start_suspended, notify_fd,
					 console_master_fd,
					 console_slave_fd, &child);
	if (ret < 0)
		goto out_fail;

	flux_runc_close_console_pair(&console_master_fd, &console_slave_fd);

	if (start_suspended) {
		ret = flux_runc_wait_pid_stopped(child);
		if (ret < 0) {
			flux_runc_reap_killed_child(child, &exit_code);
			goto out_fail;
		}
	}

	ret = flux_runc_state_mark_live(state, child, live_status, pid_file);
	if (ret < 0) {
		flux_runc_reap_killed_child(child, &exit_code);
		goto out_fail;
	}

	flux_runc_send_spawn_reply(notify_fd, child, 0);
	close(notify_fd);
	notify_fd = -1;
	return EXIT_SUCCESS;

out_fail:
	flux_runc_close_console_pair(&console_master_fd, &console_slave_fd);
	flux_runc_send_spawn_reply(notify_fd, 0, ret);
	if (notify_fd >= 0)
		close(notify_fd);
	return EXIT_FAILURE;
}

static void flux_runc_background_monitor_or_exit(struct flux_runc_state *state,
						 const char *pid_file,
						 bool start_suspended,
						 enum flux_runc_status live_status,
						 int notify_fd,
						 int console_master_fd,
						 int console_slave_fd)
{
	int ret = flux_runc_background_monitor_main(state, pid_file,
						    start_suspended,
						    live_status, notify_fd,
						    console_master_fd,
						    console_slave_fd);

	_exit(ret);
}

static int flux_runc_spawn_background_monitor(struct flux_runc_state *state,
					      const char *pid_file,
					      bool start_suspended,
					      enum flux_runc_status live_status,
					      int console_master_fd,
					      int console_slave_fd)
{
	struct flux_runc_spawn_msg msg;
	int pipefd[2];
	pid_t monitor;
	int ret;

	if (pipe(pipefd) < 0)
		return -errno;

	monitor = fork();
	if (monitor < 0) {
		ret = -errno;
		close(pipefd[0]);
		close(pipefd[1]);
		return ret;
	}

	if (monitor == 0) {
		close(pipefd[0]);
		flux_runc_background_monitor_or_exit(state, pid_file,
						     start_suspended,
						     live_status, pipefd[1],
						     console_master_fd,
						     console_slave_fd);
	}

	close(pipefd[1]);
	ret = flux_runc_read_spawn_msg(pipefd[0], &msg);
	close(pipefd[0]);
	if (ret < 0)
		return ret;
	if (msg.err < 0)
		return msg.err;

	return 0;
}

int flux_runc_runner_exec(const char *container_id, char **argv)
{
	struct flux_runc_state state;
	struct flux_launch_spec spec;
	int ret;

	(void)argv;

	if (!container_id)
		return EXIT_FAILURE;

	ret = flux_runc_state_load(&state, container_id);
	if (ret < 0)
		return EXIT_FAILURE;

	if (access(state.run_config_path, R_OK) == 0) {
		if (setenv("FLUX_RUN_CFG_FILE", state.run_config_path, 1) < 0) {
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}
	} else if (flux_launch_ensure_run_cfg_path() < 0) {
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_runc_set_signal_bridge_env();
	if (ret < 0) {
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	if (!state.exec_ring_name || !state.exec_ring_name[0]) {
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}
	if (setenv(FLUX_EXEC_RING_ENV, state.exec_ring_name, 1) < 0) {
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_launch_prepare_runc_container(
		&spec, state.id, state.bundle_dir, state.oci_config_path);
	if (ret < 0) {
		flux_launch_cleanup(&spec);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_launch_run(&spec);
	flux_launch_cleanup(&spec);
	flux_runc_state_fini(&state);
	return ret;
}

int flux_runc_runner_create_background(struct flux_runc_state *state,
				       const char *pid_file)
{
	int ret;

	ret = flux_runc_runner_prepare_start(state);
	if (ret < 0)
		return ret;

	return flux_runc_spawn_background_monitor(state, pid_file, true,
						  FLUX_RUNC_CREATED, -1, -1);
}

int flux_runc_runner_start_background(struct flux_runc_state *state,
				      const char *pid_file)
{
	int ret;

	ret = flux_runc_runner_prepare_start(state);
	if (ret < 0)
		return ret;

	return flux_runc_spawn_background_monitor(state, pid_file, false,
						  FLUX_RUNC_RUNNING, -1, -1);
}

int flux_runc_runner_start_terminal_detached(struct flux_runc_state *state,
					     const char *pid_file,
					     const char *console_socket)
{
	int master_fd = -1;
	int slave_fd = -1;
	int ret;

	if (!console_socket || !console_socket[0])
		return -EINVAL;

	ret = flux_runc_runner_prepare_start(state);
	if (ret < 0)
		return ret;

	ret = flux_runc_console_open_detached_pty(&master_fd, &slave_fd);
	if (ret < 0)
		return ret;

	ret = flux_runc_spawn_background_monitor(state, pid_file, false,
						  FLUX_RUNC_RUNNING,
						  master_fd, slave_fd);
	if (ret < 0)
		goto out;

	ret = flux_runc_console_send_fd(console_socket, master_fd);
	if (ret < 0) {
		(void)flux_runc_runner_force_kill(state);
		(void)flux_runc_runner_wait_for_exit(state, 5000);
		(void)flux_runc_state_reconcile(state);
	}

out:
	flux_runc_close_console_pair(&master_fd, &slave_fd);
	return ret;
}

int flux_runc_runner_create_terminal_detached(struct flux_runc_state *state,
					      const char *pid_file,
					      const char *console_socket)
{
	int master_fd = -1;
	int slave_fd = -1;
	int ret;

	if (!console_socket || !console_socket[0])
		return -EINVAL;

	ret = flux_runc_runner_prepare_start(state);
	if (ret < 0)
		return ret;

	ret = flux_runc_console_open_detached_pty(&master_fd, &slave_fd);
	if (ret < 0)
		return ret;

	ret = flux_runc_spawn_background_monitor(state, pid_file, true,
						  FLUX_RUNC_CREATED,
						  master_fd, slave_fd);
	if (ret < 0)
		goto out;

	ret = flux_runc_console_send_fd(console_socket, master_fd);
	if (ret < 0) {
		(void)flux_runc_runner_force_kill(state);
		(void)flux_runc_runner_wait_for_exit(state, 5000);
		(void)flux_runc_state_reconcile(state);
	}

out:
	flux_runc_close_console_pair(&master_fd, &slave_fd);
	return ret;
}

int flux_runc_runner_start_foreground(struct flux_runc_state *state,
				      const char *pid_file, int *exit_code)
{
	pid_t pid = -1;
	int ret;

	ret = flux_runc_runner_prepare_start(state);
	if (ret < 0)
		return ret;

	ret = flux_runc_fork_exec_child(state, false, -1, -1, -1, &pid);
	if (ret < 0)
		return ret;

	ret = flux_runc_state_mark_live(state, pid, FLUX_RUNC_RUNNING,
					pid_file);
	if (ret < 0) {
		flux_runc_reap_killed_child(pid, exit_code);
		return ret;
	}

	ret = flux_runc_wait_pid(pid, exit_code);
	if (ret < 0)
		return ret;

	flux_runc_state_mark_stopped(state, *exit_code);
	return 0;
}

int flux_runc_runner_continue(struct flux_runc_state *state)
{
	pid_t target_pid;
	int ret;

	if (!state || state->init_pid <= 0)
		return -ESRCH;

	ret = flux_runc_state_control_pid(state, &target_pid);
	if (ret < 0)
		return ret;

	if (kill(target_pid, SIGCONT) < 0)
		return -errno;

	state->status = FLUX_RUNC_RUNNING;
	state->exit_code = -1;
	return flux_runc_state_save(state);
}

bool flux_runc_signal_is_bridgeable(int sig)
{
	return sig > 0 && sig < NSIG;
}

bool flux_runc_signal_may_change_lifecycle(int sig)
{
	if (sig <= 0 || sig >= NSIG)
		return false;

	switch (sig) {
	case SIGCHLD:
	case SIGCONT:
	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
	case SIGURG:
	case SIGWINCH:
		return false;
	default:
		return true;
	}
}

int flux_runc_runner_signal(const struct flux_runc_state *state, int sig)
{
	pid_t target_pid;
	union sigval value;
	int ret;

	if (!state || state->init_pid <= 0)
		return -ESRCH;

	ret = flux_runc_state_control_pid(state, &target_pid);
	if (ret < 0)
		return ret;

	if (!flux_runc_signal_is_bridgeable(sig))
		return -EOPNOTSUPP;

	if (sig == SIGSEGV) {
		if (kill(target_pid, sig) < 0)
			return -errno;
		return 0;
	}

	value.sival_int = flux_signal_ctrl_pack(FLUX_SIGNAL_CTRL_KILL,
						(unsigned int)sig, 0);
	if (sigqueue(target_pid, SIGUSR1, value) < 0)
		return -errno;

	return 0;
}

int flux_runc_runner_force_kill(const struct flux_runc_state *state)
{
	bool group_signaled = false;
	pid_t pgid;

	if (!state || state->init_pid <= 0)
		return -ESRCH;

	pgid = getpgid(state->init_pid);
	if (pgid < 0) {
		if (errno != ESRCH)
			return -errno;
	} else if (!state->terminal || pgid == state->init_pid) {
		/*
		 * Background containers either inherit the monitor session
		 * (non-terminal) or create their own controlling-tty session
		 * (detached terminal). In both cases the process group must be
		 * killed to reap the multiproc child behind the bootstrap pid.
		 */
		if (kill(-pgid, SIGKILL) == 0 || errno == ESRCH)
			group_signaled = true;
		else
			return -errno;
	}

	if (kill(state->init_pid, SIGKILL) < 0 && errno != ESRCH)
		return group_signaled ? 0 : -errno;

	return 0;
}

int flux_runc_runner_wait_for_exit(struct flux_runc_state *state,
				   int timeout_ms)
{
	struct timespec sleep_for = {
		.tv_sec = 0,
		.tv_nsec = 100 * 1000 * 1000,
	};
	int elapsed = 0;
	int ret;

	if (!state)
		return -EINVAL;

	while (timeout_ms < 0 || elapsed <= timeout_ms) {
		ret = flux_runc_state_reconcile(state);
		if (ret < 0)
			return ret;
		if (state->status != FLUX_RUNC_RUNNING &&
		    state->status != FLUX_RUNC_CREATED)
			return 0;

		if (timeout_ms == 0)
			break;

		if (nanosleep(&sleep_for, NULL) < 0 && errno != EINTR)
			return -errno;

		elapsed += 100;
	}

	return -ETIMEDOUT;
}
