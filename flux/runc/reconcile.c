#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <flux.h>

#include "runc.h"

#define FLUX_RUNC_STOPPED_FALLBACK_EXIT 255

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

static int flux_runc_state_mark_stopped(struct flux_runc_state *state)
{
	state->status = FLUX_RUNC_STOPPED;
	state->init_pid = 0;
	if (state->exit_code < 0)
		state->exit_code = FLUX_RUNC_STOPPED_FALLBACK_EXIT;
	return flux_runc_state_save(state);
}

int flux_runc_state_reconcile(struct flux_runc_state *state)
{
	char proc_state;
	int ret;

	if (state->status != FLUX_RUNC_RUNNING &&
	    state->status != FLUX_RUNC_CREATED)
		return 0;

	if (state->init_pid <= 0) {
		if (state->status == FLUX_RUNC_CREATED)
			return 0;
		return flux_runc_state_mark_stopped(state);
	}

	if (kill(state->init_pid, 0) == 0 || errno == EPERM) {
		ret = flux_runc_read_proc_state(state->init_pid, &proc_state);
		if (ret == -ENOENT || ret == -ESRCH)
			return flux_runc_state_mark_stopped(state);
		if (ret == 0 && proc_state == 'Z')
			return flux_runc_state_mark_stopped(state);
		return 0;
	}

	if (errno != ESRCH)
		return -errno;

	return flux_runc_state_mark_stopped(state);
}
