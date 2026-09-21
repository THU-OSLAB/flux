#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include <flux.h>

#include "runc.h"

#define FLUX_RUNC_ROOT_DIR "/run/flux-runc"
#define FLUX_RUNC_STATUS_FILE "status.json"
#define FLUX_RUNC_OCI_CONFIG_FILE "oci-config.json"
#define FLUX_RUNC_RUN_CONFIG_FILE "run-config.json"
#define FLUX_RUNC_RESOURCES_FILE "resources.json"
#define FLUX_RUNC_EXEC_SESSION_DIR "sessions"
static void flux_runc_free_string(char **value)
{
	if (!value || !*value)
		return;

	free(*value);
	*value = NULL;
}

static int flux_runc_strdup(char **dst, const char *src)
{
	char *copy;

	copy = strdup(src);
	if (!copy)
		return -1;

	free(*dst);
	*dst = copy;
	return 0;
}

static int flux_runc_set_fmt(char **dst, const char *fmt, ...)
{
	va_list ap;
	va_list ap_copy;
	char *buf;
	int len;

	va_start(ap, fmt);
	va_copy(ap_copy, ap);
	len = vsnprintf(NULL, 0, fmt, ap_copy);
	va_end(ap_copy);
	if (len < 0) {
		va_end(ap);
		return -1;
	}

	buf = malloc((size_t)len + 1);
	if (!buf) {
		va_end(ap);
		return -1;
	}

	if (vsnprintf(buf, (size_t)len + 1, fmt, ap) != len) {
		va_end(ap);
		free(buf);
		return -1;
	}
	va_end(ap);

	free(*dst);
	*dst = buf;
	return 0;
}

static int flux_runc_host_mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len;

	if (!path || path[0] != '/')
		return -EINVAL;

	if (!strcmp(path, "/"))
		return 0;

	len = strlen(path);
	if (len >= sizeof(tmp))
		return -ENAMETOOLONG;

	memcpy(tmp, path, len + 1);
	for (size_t i = 1; i < len; i++) {
		if (tmp[i] != '/')
			continue;

		tmp[i] = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -errno;
		tmp[i] = '/';
	}

	if (mkdir(tmp, mode) < 0 && errno != EEXIST)
		return -errno;

	return 0;
}

static int flux_runc_copy_file(const char *src, const char *dst)
{
	char buf[4096];
	ssize_t nr;
	int in_fd = -1;
	int out_fd = -1;
	int ret = -1;

	in_fd = open(src, O_RDONLY | O_CLOEXEC);
	if (in_fd < 0)
		return -errno;

	out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out_fd < 0) {
		ret = -errno;
		goto out;
	}

	while ((nr = read(in_fd, buf, sizeof(buf))) > 0) {
		char *pos = buf;

		while (nr > 0) {
			ssize_t nw = write(out_fd, pos, (size_t)nr);

			if (nw < 0) {
				ret = -errno;
				goto out;
			}

			pos += nw;
			nr -= nw;
		}
	}

	if (nr < 0) {
		ret = -errno;
		goto out;
	}

	ret = 0;
out:
	if (out_fd >= 0)
		close(out_fd);
	if (in_fd >= 0)
		close(in_fd);
	return ret;
}

static int flux_runc_write_atomic(const char *path, const char *buf, size_t len)
{
	char tmp[PATH_MAX];
	ssize_t nw;
	int fd;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >=
	    (int)sizeof(tmp))
		return -ENAMETOOLONG;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;

	while (len > 0) {
		nw = write(fd, buf, len);
		if (nw < 0) {
			int err = -errno;

			close(fd);
			unlink(tmp);
			return err;
		}

		buf += nw;
		len -= (size_t)nw;
	}

	if (close(fd) < 0) {
		int err = -errno;

		unlink(tmp);
		return err;
	}

	if (rename(tmp, path) < 0) {
		int err = -errno;

		unlink(tmp);
		return err;
	}

	return 0;
}

static int flux_runc_read_file(const char *path, char **out)
{
	char *buf = NULL;
	ssize_t len;
	size_t off = 0;
	int fd = -1;
	int ret = -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	len = lseek(fd, 0, SEEK_END);
	if (len < 0) {
		ret = -errno;
		goto out;
	}

	if (lseek(fd, 0, SEEK_SET) < 0) {
		ret = -errno;
		goto out;
	}

	buf = malloc((size_t)len + 1);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	while (off < (size_t)len) {
		ssize_t nr = read(fd, buf + off, (size_t)len - off);

		if (nr < 0) {
			ret = -errno;
			goto out;
		}
		if (nr == 0) {
			ret = -EIO;
			goto out;
		}
		off += (size_t)nr;
	}

	buf[len] = '\0';
	*out = buf;
	buf = NULL;
	ret = 0;
out:
	free(buf);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int flux_runc_remove_tree(const char *path)
{
	struct stat st;

	if (lstat(path, &st) < 0) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	if (S_ISDIR(st.st_mode)) {
		DIR *dir;
		struct dirent *de;
		int ret;

		dir = opendir(path);
		if (!dir)
			return -errno;

		while ((de = readdir(dir)) != NULL) {
			char child[PATH_MAX];

			if (!strcmp(de->d_name, ".") ||
			    !strcmp(de->d_name, ".."))
				continue;

			if (snprintf(child, sizeof(child), "%s/%s", path,
				     de->d_name) >= (int)sizeof(child)) {
				closedir(dir);
				return -ENAMETOOLONG;
			}

			ret = flux_runc_remove_tree(child);
			if (ret < 0) {
				closedir(dir);
				return ret;
			}
		}

		closedir(dir);
		if (rmdir(path) < 0)
			return -errno;
		return 0;
	}

	if (unlink(path) < 0)
		return -errno;

	return 0;
}

static int flux_runc_validate_id(const char *id)
{
	size_t i;

	if (!id || !id[0])
		return -EINVAL;

	for (i = 0; id[i]; i++) {
		char ch = id[i];

		if (isalnum((unsigned char)ch) || ch == '_' || ch == '-' ||
		    ch == '.')
			continue;
		return -EINVAL;
	}

	return 0;
}

static int flux_runc_try_state_root(const char *path, char **out)
{
	int ret;

	ret = flux_runc_host_mkdir_p(path, 0755);
	if (ret < 0)
		return ret;

	if (access(path, W_OK | X_OK) < 0)
		return -errno;

	return flux_runc_strdup(out, path);
}

static int flux_runc_probe_existing_state_root(const char *path, const char *id,
					       char **out)
{
	char state_dir[PATH_MAX];

	if (!path || !id)
		return -EINVAL;

	if (snprintf(state_dir, sizeof(state_dir), "%s/%s", path, id) >=
	    (int)sizeof(state_dir))
		return -ENAMETOOLONG;

	if (access(state_dir, F_OK) < 0) {
		if (errno == ENOENT)
			return -ENOENT;
		return -errno;
	}

	return flux_runc_strdup(out, path);
}

static int flux_runc_resolve_state_root_for_create(char **out)
{
	char user_root[PATH_MAX];
	char *tmp_root;
	const char *env_root = getenv("FLUX_RUNC_ROOT");
	uid_t uid = getuid();
	int ret;

	if (env_root && env_root[0]) {
		if (env_root[0] != '/')
			return -EINVAL;
		return flux_runc_try_state_root(env_root, out);
	}

	ret = flux_runc_try_state_root(FLUX_RUNC_ROOT_DIR, out);
	if (ret == 0)
		return 0;
	if (ret != -EACCES && ret != -EROFS && ret != -EPERM && ret != -ENOENT)
		return ret;

	if (snprintf(user_root, sizeof(user_root), "/run/user/%lu/flux-runc",
		     (unsigned long)uid) >= (int)sizeof(user_root))
		return -ENAMETOOLONG;

	ret = flux_runc_try_state_root(user_root, out);
	if (ret == 0)
		return 0;
	if (ret != -EACCES && ret != -EROFS && ret != -EPERM && ret != -ENOENT)
		return ret;

	tmp_root = flux_user_scoped_path_strdup("/tmp/flux-runc");
	if (!tmp_root)
		return -ENOMEM;

	ret = flux_runc_try_state_root(tmp_root, out);
	free(tmp_root);
	return ret;
}

static int flux_runc_resolve_state_root_for_load(char **out, const char *id)
{
	char user_root[PATH_MAX];
	char *tmp_root;
	const char *env_root = getenv("FLUX_RUNC_ROOT");
	uid_t uid = getuid();
	int ret;

	if (env_root && env_root[0]) {
		if (env_root[0] != '/')
			return -EINVAL;
		return flux_runc_probe_existing_state_root(env_root, id, out);
	}

	ret = flux_runc_probe_existing_state_root(FLUX_RUNC_ROOT_DIR, id, out);
	if (ret == 0 || ret != -ENOENT)
		return ret;

	if (snprintf(user_root, sizeof(user_root), "/run/user/%lu/flux-runc",
		     (unsigned long)uid) >= (int)sizeof(user_root))
		return -ENAMETOOLONG;

	ret = flux_runc_probe_existing_state_root(user_root, id, out);
	if (ret == 0 || ret != -ENOENT)
		return ret;

	tmp_root = flux_user_scoped_path_strdup("/tmp/flux-runc");
	if (!tmp_root)
		return -ENOMEM;

	ret = flux_runc_probe_existing_state_root(tmp_root, id, out);
	free(tmp_root);
	return ret;
}

static int flux_runc_state_init_paths(struct flux_runc_state *state,
				      const char *id, bool for_load)
{
	int ret;

	ret = flux_runc_validate_id(id);
	if (ret < 0)
		return ret;

	if (for_load)
		ret = flux_runc_resolve_state_root_for_load(&state->state_root,
							    id);
	else
		ret = flux_runc_resolve_state_root_for_create(
			&state->state_root);
	if (ret < 0)
		return ret;

	if (flux_runc_strdup(&state->id, id) < 0)
		return -ENOMEM;

	if (flux_runc_set_fmt(&state->state_dir, "%s/%s", state->state_root,
			      state->id) < 0)
		return -ENOMEM;

	if (flux_runc_set_fmt(&state->oci_config_path, "%s/%s",
			      state->state_dir, FLUX_RUNC_OCI_CONFIG_FILE) < 0)
		return -ENOMEM;

	if (flux_runc_set_fmt(&state->run_config_path, "%s/%s",
			      state->state_dir, FLUX_RUNC_RUN_CONFIG_FILE) < 0)
		return -ENOMEM;
	if (flux_runc_set_fmt(&state->resources_path, "%s/%s",
			      state->state_dir, FLUX_RUNC_RESOURCES_FILE) < 0)
		return -ENOMEM;

	return 0;
}

static int flux_runc_state_status_path(const struct flux_runc_state *state,
				       char *buf, size_t size)
{
	if (snprintf(buf, size, "%s/%s", state->state_dir,
		     FLUX_RUNC_STATUS_FILE) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

int flux_runc_state_exec_session_root(const struct flux_runc_state *state,
				      char *buf, size_t size)
{
	uid_t euid;

	(void)state;

	if (!buf || size == 0)
		return -EINVAL;

	euid = geteuid();
	if (euid == 0) {
		if (snprintf(buf, size, "/run/flux-runc") >= (int)size)
			return -ENAMETOOLONG;
		return 0;
	}

	if (snprintf(buf, size, "/run/user/%lu/flux-runc",
		     (unsigned long)euid) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

int flux_runc_state_set_cgroup_path(struct flux_runc_state *state,
				    const char *cgroup_path)
{
	if (!state)
		return -EINVAL;
	if (!cgroup_path || !cgroup_path[0]) {
		flux_runc_free_string(&state->cgroup_path);
		return 0;
	}
	return flux_runc_strdup(&state->cgroup_path, cgroup_path) < 0 ?
		       -ENOMEM : 0;
}

static int flux_runc_json_escape(FILE *stream, const char *value)
{
	const unsigned char *p;

	if (fputc('"', stream) == EOF)
		return -1;

	for (p = (const unsigned char *)value; *p; p++) {
		switch (*p) {
		case '\\':
		case '"':
			if (fprintf(stream, "\\%c", *p) < 0)
				return -1;
			break;
		case '\n':
			if (fputs("\\n", stream) == EOF)
				return -1;
			break;
		case '\r':
			if (fputs("\\r", stream) == EOF)
				return -1;
			break;
		case '\t':
			if (fputs("\\t", stream) == EOF)
				return -1;
			break;
		default:
			if (*p < 0x20) {
				if (fprintf(stream, "\\u%04x", *p) < 0)
					return -1;
			} else if (fputc(*p, stream) == EOF) {
				return -1;
			}
			break;
		}
	}

	if (fputc('"', stream) == EOF)
		return -1;

	return 0;
}

static int flux_runc_state_emit_json(const struct flux_runc_state *state,
				     FILE *stream)
{
	if (fputs("{\n  \"ociVersion\": \"1.3.0\",\n  \"id\": ", stream) == EOF)
		return -1;
	if (flux_runc_json_escape(stream, state->id ?: "") < 0)
		return -1;
	if (fputs(",\n  \"status\": ", stream) == EOF)
		return -1;
	if (flux_runc_json_escape(stream,
				  flux_runc_status_name(state->status)) < 0)
		return -1;
	if (fputs(",\n  \"pid\": ", stream) == EOF)
		return -1;
	if (fprintf(stream, "%ld",
		    (long)(state->status == FLUX_RUNC_STOPPED ? 0 :
					       state->init_pid)) < 0)
		return -1;
	if (fputs(",\n  \"bundle\": ", stream) == EOF)
		return -1;
	if (flux_runc_json_escape(stream, state->bundle_dir ?: "") < 0)
		return -1;
	if (fputs(",\n  \"pidFile\": ", stream) == EOF)
		return -1;
	if (state->pid_file_path) {
		if (flux_runc_json_escape(stream, state->pid_file_path) < 0)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}
	if (fputs(",\n  \"execRingShm\": ", stream) == EOF)
		return -1;
	if (state->exec_ring_name) {
		if (flux_runc_json_escape(stream, state->exec_ring_name) < 0)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}
	if (fputs(",\n  \"cgroupsPath\": ", stream) == EOF)
		return -1;
	if (state->cgroup_path) {
		if (flux_runc_json_escape(stream, state->cgroup_path) < 0)
			return -1;
	} else if (fputs("null", stream) == EOF) {
		return -1;
	}
	if (fputs(",\n  \"exitCode\": ", stream) == EOF)
		return -1;
	if (fprintf(stream, "%d", state->exit_code) < 0)
		return -1;
	if (fputs(",\n  \"terminal\": ", stream) == EOF)
		return -1;
	if (fputs(state->terminal ? "true" : "false", stream) == EOF)
		return -1;
	if (fputs("\n}\n", stream) == EOF)
		return -1;

	return 0;
}

static const char *flux_runc_json_find_key(const char *json, const char *key)
{
	char pattern[64];

	if (snprintf(pattern, sizeof(pattern), "\"%s\":", key) >=
	    (int)sizeof(pattern))
		return NULL;

	return strstr(json, pattern);
}

static int flux_runc_json_get_string(const char *json, const char *key,
				     char **out)
{
	const char *pos;
	const char *end;
	char *value;
	size_t len;

	pos = flux_runc_json_find_key(json, key);
	if (!pos)
		return -EINVAL;

	pos = strchr(pos, ':');
	if (!pos)
		return -EINVAL;
	pos++;
	while (*pos == ' ' || *pos == '\t')
		pos++;
	if (*pos != '"')
		return -EINVAL;
	pos++;

	end = strchr(pos, '"');
	if (!end)
		return -EINVAL;

	len = (size_t)(end - pos);
	value = malloc(len + 1);
	if (!value)
		return -ENOMEM;

	memcpy(value, pos, len);
	value[len] = '\0';
	free(*out);
	*out = value;
	return 0;
}

static int flux_runc_json_get_optional_string(const char *json, const char *key,
					      char **out)
{
	const char *pos;

	pos = flux_runc_json_find_key(json, key);
	if (!pos)
		return 0;

	pos = strchr(pos, ':');
	if (!pos)
		return -EINVAL;
	pos++;
	while (*pos == ' ' || *pos == '\t')
		pos++;

	if (!strncmp(pos, "null", 4)) {
		free(*out);
		*out = NULL;
		return 0;
	}

	return flux_runc_json_get_string(json, key, out);
}

static int flux_runc_json_get_int(const char *json, const char *key, int *out)
{
	const char *pos;
	char *end;
	long value;

	pos = flux_runc_json_find_key(json, key);
	if (!pos)
		return -EINVAL;

	pos = strchr(pos, ':');
	if (!pos)
		return -EINVAL;
	pos++;
	while (*pos == ' ' || *pos == '\t')
		pos++;

	errno = 0;
	value = strtol(pos, &end, 10);
	if (errno || end == pos)
		return -EINVAL;

	*out = (int)value;
	return 0;
}

static int flux_runc_json_get_bool(const char *json, const char *key, bool *out)
{
	const char *pos;

	pos = flux_runc_json_find_key(json, key);
	if (!pos)
		return -EINVAL;

	pos = strchr(pos, ':');
	if (!pos)
		return -EINVAL;
	pos++;
	while (*pos == ' ' || *pos == '\t')
		pos++;

	if (!strncmp(pos, "true", 4)) {
		*out = true;
		return 0;
	}
	if (!strncmp(pos, "false", 5)) {
		*out = false;
		return 0;
	}

	return -EINVAL;
}

static int flux_runc_parse_status_json(const char *json,
				       struct flux_runc_state *state)
{
	char *status = NULL;
	int pid = 0;
	int ret;

	ret = flux_runc_json_get_string(json, "id", &status);
	if (ret < 0)
		return ret;
	ret = strcmp(status, state->id) ? -EINVAL : 0;
	free(status);
	status = NULL;
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_string(json, "bundle", &state->bundle_dir);
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_optional_string(json, "pidFile",
						 &state->pid_file_path);
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_optional_string(json, "execRingShm",
						 &state->exec_ring_name);
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_optional_string(json, "cgroupsPath",
						 &state->cgroup_path);
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_string(json, "status", &status);
	if (ret < 0)
		return ret;

	if (!strcmp(status, "creating"))
		state->status = FLUX_RUNC_CREATING;
	else if (!strcmp(status, "created"))
		state->status = FLUX_RUNC_CREATED;
	else if (!strcmp(status, "running"))
		state->status = FLUX_RUNC_RUNNING;
	else if (!strcmp(status, "stopped"))
		state->status = FLUX_RUNC_STOPPED;
	else
		ret = -EINVAL;

	free(status);
	status = NULL;
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_int(json, "pid", &pid);
	if (ret < 0)
		return ret;
	state->init_pid = (pid_t)pid;

	ret = flux_runc_json_get_int(json, "exitCode", &state->exit_code);
	if (ret < 0)
		return ret;

	ret = flux_runc_json_get_bool(json, "terminal", &state->terminal);
	if (ret < 0)
		return ret;

	if (!state->bundle_dir)
		return -EINVAL;

	return 0;
}

const char *flux_runc_status_name(enum flux_runc_status status)
{
	switch (status) {
	case FLUX_RUNC_CREATING:
		return "creating";
	case FLUX_RUNC_CREATED:
		return "created";
	case FLUX_RUNC_RUNNING:
		return "running";
	case FLUX_RUNC_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

void flux_runc_state_reset(struct flux_runc_state *state)
{
	memset(state, 0, sizeof(*state));
	state->exit_code = -1;
}

void flux_runc_state_fini(struct flux_runc_state *state)
{
	if (!state)
		return;

	flux_runc_free_string(&state->id);
	flux_runc_free_string(&state->bundle_dir);
	flux_runc_free_string(&state->state_root);
	flux_runc_free_string(&state->state_dir);
	flux_runc_free_string(&state->oci_config_path);
	flux_runc_free_string(&state->run_config_path);
	flux_runc_free_string(&state->resources_path);
	flux_runc_free_string(&state->pid_file_path);
	flux_runc_free_string(&state->exec_ring_name);
	flux_runc_free_string(&state->cgroup_path);
	flux_runc_state_reset(state);
}

int flux_runc_state_init_exec_ring(struct flux_runc_state *state)
{
	uid_t uid;

	if (!state || !state->id)
		return -EINVAL;
	if (state->exec_ring_name)
		return 0;

	uid = getuid();
	if (flux_runc_set_fmt(&state->exec_ring_name, "/flux-runc-%lu-%s-exec",
			      (unsigned long)uid, state->id) < 0)
		return -ENOMEM;

	return 0;
}

int flux_runc_state_prepare_new(struct flux_runc_state *state, const char *id,
				const char *bundle_dir, bool terminal)
{
	bool state_dir_created = false;
	int ret;

	flux_runc_state_reset(state);

	ret = flux_runc_state_init_paths(state, id, false);
	if (ret < 0)
		goto err;

	if (mkdir(state->state_dir, 0755) < 0) {
		ret = errno == EEXIST ? -EEXIST : -errno;
		goto err;
	}
	state_dir_created = true;

	if (flux_runc_strdup(&state->bundle_dir, bundle_dir) < 0) {
		ret = -ENOMEM;
		goto err;
	}

	state->status = FLUX_RUNC_CREATING;
	state->terminal = terminal;
	state->init_pid = 0;
	state->exit_code = -1;

	ret = flux_runc_state_init_exec_ring(state);
	if (ret < 0)
		goto err;

	ret = flux_runc_state_save(state);
	if (ret < 0)
		goto err;

	return 0;
err:
	if (state_dir_created && state->state_dir)
		(void)flux_runc_remove_tree(state->state_dir);
	flux_runc_state_fini(state);
	return ret;
}

int flux_runc_state_load(struct flux_runc_state *state, const char *id)
{
	char status_path[PATH_MAX];
	char *json = NULL;
	int ret;

	flux_runc_state_reset(state);

	ret = flux_runc_state_init_paths(state, id, true);
	if (ret < 0)
		goto err;

	ret = flux_runc_state_status_path(state, status_path,
					  sizeof(status_path));
	if (ret < 0)
		goto err;

	ret = flux_runc_read_file(status_path, &json);
	if (ret < 0)
		goto err;

	ret = flux_runc_parse_status_json(json, state);
	if (ret < 0)
		goto err;

	free(json);
	return 0;
err:
	free(json);
	flux_runc_state_fini(state);
	return ret;
}

int flux_runc_state_save(const struct flux_runc_state *state)
{
	char status_path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	FILE *mem;
	int ret;

	ret = flux_runc_state_status_path(state, status_path,
					  sizeof(status_path));
	if (ret < 0)
		return ret;

	mem = open_memstream(&buf, &len);
	if (!mem)
		return -errno;

	if (flux_runc_state_emit_json(state, mem) < 0) {
		fclose(mem);
		free(buf);
		return -EIO;
	}

	if (fclose(mem) < 0) {
		free(buf);
		return -errno;
	}

	ret = flux_runc_write_atomic(status_path, buf, len);
	free(buf);
	return ret;
}

int flux_runc_state_snapshot_bundle_config(const struct flux_runc_state *state)
{
	char src[PATH_MAX];

	if (snprintf(src, sizeof(src), "%s/config.json", state->bundle_dir) >=
	    (int)sizeof(src))
		return -ENAMETOOLONG;

	return flux_runc_copy_file(src, state->oci_config_path);
}

int flux_runc_state_snapshot_run_config(const struct flux_runc_state *state,
					const char *path)
{
	return flux_runc_copy_file(path, state->run_config_path);
}

int flux_runc_state_cleanup_artifacts(const struct flux_runc_state *state)
{
	int ret;

	if (!state)
		return 0;

	if (state->pid_file_path && state->pid_file_path[0] &&
	    unlink(state->pid_file_path) < 0 && errno != ENOENT)
		return -errno;

	if (state->exec_ring_name && state->exec_ring_name[0] &&
	    shm_unlink(state->exec_ring_name) < 0 && errno != ENOENT)
		return -errno;

	ret = flux_runc_cgroup_destroy(state);
	if (ret < 0)
		return ret;

	return 0;
}

int flux_runc_state_remove(const struct flux_runc_state *state)
{
	return flux_runc_remove_tree(state->state_dir);
}

int flux_runc_state_print(const struct flux_runc_state *state, FILE *stream)
{
	return flux_runc_state_emit_json(state, stream);
}
