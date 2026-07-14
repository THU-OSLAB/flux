#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <flux.h>

#include "../runc.h"

#define JSMN_HEADER
#include <utils/jsmn.h>

#define FLUX_RUNC_EXEC_TOKEN_MAX 512
#define FLUX_RUNC_EXEC_WAIT_TIMEOUT_ENV "FLUX_RUNC_EXEC_WAIT_TIMEOUT_MS"
#define FLUX_RUNC_EXEC_WAIT_TIMEOUT_MS 30000
#define FLUX_RUNC_EXEC_SESSION_DIR "sessions"

struct flux_runc_exec_session {
	uint32_t id;
	char dir[PATH_MAX];
	bool active;
	pid_t stdin_pid;
	pid_t stdout_pid;
	pid_t stderr_pid;
};

static void flux_runc_exec_spec_fini(struct flux_runc_exec_spec *spec)
{
	int i;

	if (!spec)
		return;

	free(spec->filename);
	free(spec->cwd);
	for (i = 0; i < spec->argc; i++)
		free(spec->argv[i]);
	free(spec->argv);
	for (i = 0; i < spec->env_count; i++)
		free(spec->env[i]);
	free(spec->env);
	memset(spec, 0, sizeof(*spec));
}

static int flux_runc_exec_strdup(char **dst, const char *src)
{
	char *copy;

	if (!src)
		return -EINVAL;

	copy = strdup(src);
	if (!copy)
		return -ENOMEM;

	free(*dst);
	*dst = copy;
	return 0;
}

static int flux_runc_exec_write_pid_file(const char *path, pid_t pid)
{
	char buf[32];
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

static int flux_runc_exec_append_string(char ***list, int *count,
					const char *value)
{
	char **tmp;
	char *copy;

	copy = strdup(value);
	if (!copy)
		return -ENOMEM;

	tmp = realloc(*list, sizeof(*tmp) * (size_t)(*count + 1));
	if (!tmp) {
		free(copy);
		return -ENOMEM;
	}

	tmp[*count] = copy;
	*list = tmp;
	(*count)++;
	return 0;
}

static int flux_runc_exec_replace_env(char ***envp, int *envc,
				      const char *entry)
{
	const char *eq;
	size_t key_len;
	int i;

	eq = strchr(entry, '=');
	key_len = eq ? (size_t)(eq - entry) : strlen(entry);

	for (i = 0; i < *envc; i++) {
		if (!strncmp((*envp)[i], entry, key_len) &&
		    (*envp)[i][key_len] == '=') {
			char *copy = strdup(entry);

			if (!copy)
				return -ENOMEM;
			free((*envp)[i]);
			(*envp)[i] = copy;
			return 0;
		}
	}

	return flux_runc_exec_append_string(envp, envc, entry);
}

static int flux_runc_exec_load_oci_defaults(struct flux_runc_state *state,
					    struct flux_runc_exec_spec *spec)
{
	const struct flux_oci_cfg *oci;
	int i;
	int ret;

	ret = flux_runc_set_bundle_config(state->bundle_dir,
					  state->oci_config_path);
	if (ret < 0)
		return ret;

	ret = flux_runc_load_bundle();
	if (ret < 0)
		return ret;

	oci = flux_oci_cfg_get();
	if (!oci)
		return -EINVAL;

	if (oci->cwd) {
		ret = flux_runc_exec_strdup(&spec->cwd, oci->cwd);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < oci->env_num; i++) {
		ret = flux_runc_exec_append_string(&spec->env, &spec->env_count,
						   oci->env[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static bool flux_runc_exec_token_eq(const char *json, const jsmntok_t *tok,
				    const char *expect)
{
	size_t len;

	if (!tok || tok->type != JSMN_STRING)
		return false;

	len = strlen(expect);
	return (size_t)(tok->end - tok->start) == len &&
	       strncmp(json + tok->start, expect, len) == 0;
}

static int flux_runc_exec_token_skip(const jsmntok_t *tokens, int index)
{
	int i;
	int size;

	switch (tokens[index].type) {
	case JSMN_PRIMITIVE:
	case JSMN_STRING:
		return index + 1;
	case JSMN_ARRAY:
		size = tokens[index].size;
		index++;
		for (i = 0; i < size; i++)
			index = flux_runc_exec_token_skip(tokens, index);
		return index;
	case JSMN_OBJECT:
		size = tokens[index].size;
		index++;
		for (i = 0; i < size; i++) {
			index = flux_runc_exec_token_skip(tokens, index);
			index = flux_runc_exec_token_skip(tokens, index);
		}
		return index;
	default:
		return index + 1;
	}
}

static int flux_runc_exec_token_str(const char *json, const jsmntok_t *tok,
				    char **out)
{
	size_t len;
	char *copy;

	if (!tok || tok->type != JSMN_STRING)
		return -EINVAL;

	len = (size_t)(tok->end - tok->start);
	copy = malloc(len + 1);
	if (!copy)
		return -ENOMEM;

	memcpy(copy, json + tok->start, len);
	copy[len] = '\0';
	free(*out);
	*out = copy;
	return 0;
}

static int flux_runc_exec_parse_int(const char *json, const jsmntok_t *tok,
				    unsigned int *out)
{
	char buf[32];
	size_t len;
	char *end = NULL;
	unsigned long value;

	if (!tok || tok->type != JSMN_PRIMITIVE)
		return -EINVAL;

	len = (size_t)(tok->end - tok->start);
	if (len >= sizeof(buf))
		return -EINVAL;

	memcpy(buf, json + tok->start, len);
	buf[len] = '\0';
	value = strtoul(buf, &end, 10);
	if (!end || *end != '\0')
		return -EINVAL;

	*out = (unsigned int)value;
	return 0;
}

static int flux_runc_exec_parse_bool(const char *json, const jsmntok_t *tok,
				     bool *out)
{
	size_t len;

	if (!tok || tok->type != JSMN_PRIMITIVE || !out)
		return -EINVAL;

	len = (size_t)(tok->end - tok->start);
	if (len == 4 && strncmp(json + tok->start, "true", 4) == 0) {
		*out = true;
		return 0;
	}
	if (len == 5 && strncmp(json + tok->start, "false", 5) == 0) {
		*out = false;
		return 0;
	}

	return -EINVAL;
}

static int flux_runc_exec_parse_string_array(const char *json,
					     const jsmntok_t *tokens, int index,
					     char ***list, int *count)
{
	int i;

	if (tokens[index].type != JSMN_ARRAY)
		return -EINVAL;

	for (i = 0; i < tokens[index].size; i++) {
		char *tmp = NULL;
		int ret;

		ret = flux_runc_exec_token_str(json, &tokens[index + 1 + i],
					       &tmp);
		if (ret < 0)
			return ret;
		ret = flux_runc_exec_append_string(list, count, tmp);
		free(tmp);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int flux_runc_exec_parse_env_array(const char *json,
					  const jsmntok_t *tokens, int index,
					  char ***envp, int *envc)
{
	int i;

	if (tokens[index].type != JSMN_ARRAY)
		return -EINVAL;

	for (i = 0; i < tokens[index].size; i++) {
		char *tmp = NULL;
		int ret;

		ret = flux_runc_exec_token_str(json, &tokens[index + 1 + i],
					       &tmp);
		if (ret < 0)
			return ret;
		ret = flux_runc_exec_replace_env(envp, envc, tmp);
		free(tmp);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int flux_runc_exec_parse_process_json(const char *path,
					     struct flux_runc_exec_spec *spec)
{
	jsmn_parser parser;
	jsmntok_t tokens[FLUX_RUNC_EXEC_TOKEN_MAX];
	char *json = NULL;
	int ret;
	int i;
	int tok;

	ret = 0;
	{
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t nr;
		size_t cap = 4096;
		size_t len = 0;

		if (fd < 0)
			return -errno;
		json = malloc(cap);
		if (!json) {
			close(fd);
			return -ENOMEM;
		}
		while ((nr = read(fd, json + len, cap - len)) != 0) {
			char *tmp;

			if (nr < 0) {
				ret = -errno;
				close(fd);
				free(json);
				return ret;
			}
			len += (size_t)nr;
			if (len < cap)
				continue;
			cap *= 2;
			tmp = realloc(json, cap);
			if (!tmp) {
				close(fd);
				free(json);
				return -ENOMEM;
			}
			json = tmp;
		}
		if (len == cap) {
			char *tmp = realloc(json, cap + 1);

			if (!tmp) {
				close(fd);
				free(json);
				return -ENOMEM;
			}
			json = tmp;
		}
		close(fd);
		json[len] = '\0';
	}

	jsmn_init(&parser);
	ret = jsmn_parse(&parser, json, strlen(json), tokens,
			 FLUX_RUNC_EXEC_TOKEN_MAX);
	if (ret < 0) {
		free(json);
		return -EINVAL;
	}
	if (ret <= 0 || tokens[0].type != JSMN_OBJECT) {
		free(json);
		return -EINVAL;
	}

	tok = 1;
	for (i = 0; i < tokens[0].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_runc_exec_token_eq(json, &tokens[key], "args")) {
			ret = flux_runc_exec_parse_string_array(
				json, tokens, value, &spec->argv, &spec->argc);
		} else if (flux_runc_exec_token_eq(json, &tokens[key], "env")) {
			ret = flux_runc_exec_parse_env_array(json, tokens,
							     value, &spec->env,
							     &spec->env_count);
		} else if (flux_runc_exec_token_eq(json, &tokens[key], "cwd")) {
			ret = flux_runc_exec_token_str(json, &tokens[value],
						       &spec->cwd);
		} else if (flux_runc_exec_token_eq(json, &tokens[key],
						   "terminal")) {
			ret = flux_runc_exec_parse_bool(json, &tokens[value],
						       &spec->terminal);
		} else if (flux_runc_exec_token_eq(json, &tokens[key],
						   "user")) {
			int utok = value + 1;
			int j;

			if (tokens[value].type != JSMN_OBJECT) {
				ret = -EINVAL;
			} else {
				for (j = 0; j < tokens[value].size; j++) {
					int ukey = utok;
					int uvalue = ukey + 1;
					unsigned int parsed = 0;

					if (flux_runc_exec_token_eq(
						    json, &tokens[ukey],
						    "uid")) {
						ret = flux_runc_exec_parse_int(
							json, &tokens[uvalue],
							&parsed);
						spec->uid = (uid_t)parsed;
						spec->has_user = true;
					} else if (flux_runc_exec_token_eq(
							   json, &tokens[ukey],
							   "gid")) {
						ret = flux_runc_exec_parse_int(
							json, &tokens[uvalue],
							&parsed);
						spec->gid = (gid_t)parsed;
						spec->has_user = true;
					}
					if (ret < 0)
						break;
					utok = flux_runc_exec_token_skip(
						tokens, uvalue);
				}
			}
		}

		if (ret < 0)
			break;
		tok = flux_runc_exec_token_skip(tokens, value);
	}

	free(json);
	return ret;
}

static int flux_runc_exec_parse_user(const char *spec,
				     struct flux_runc_exec_spec *exec)
{
	char *copy;
	char *sep;
	char *end = NULL;
	unsigned long uid;
	unsigned long gid;

	if (!spec || !spec[0])
		return 0;

	copy = strdup(spec);
	if (!copy)
		return -ENOMEM;

	sep = strchr(copy, ':');
	if (sep)
		*sep++ = '\0';

	uid = strtoul(copy, &end, 10);
	if (!end || *end != '\0') {
		free(copy);
		return -EINVAL;
	}
	exec->uid = (uid_t)uid;
	exec->gid = (gid_t)uid;
	exec->has_user = true;

	if (sep) {
		end = NULL;
		gid = strtoul(sep, &end, 10);
		if (!end || *end != '\0') {
			free(copy);
			return -EINVAL;
		}
		exec->gid = (gid_t)gid;
	}

	free(copy);
	return 0;
}

static const char *flux_runc_exec_env_lookup(char **env, int envc,
					     const char *key)
{
	size_t key_len;
	int i;

	if (!env || !key || !key[0])
		return NULL;

	key_len = strlen(key);
	for (i = 0; i < envc; i++) {
		if (!strncmp(env[i], key, key_len) && env[i][key_len] == '=')
			return env[i] + key_len + 1;
	}

	return NULL;
}

static int flux_runc_exec_build_candidate_path(const char *rootfs,
					       const char *prefix,
					       const char *name, char *buf,
					       size_t size)
{
	const char *base = prefix && prefix[0] ? prefix : "";

	if (!rootfs || !rootfs[0] || !name || !name[0] || !buf || size == 0)
		return -EINVAL;

	if (name[0] == '/') {
		if (snprintf(buf, size, "%s%s", rootfs, name) >= (int)size)
			return -ENAMETOOLONG;
		return 0;
	}

	if (base[0] && base[0] != '/') {
		if (snprintf(buf, size, "%s/%s/%s", rootfs, base, name) >=
		    (int)size)
			return -ENAMETOOLONG;
		return 0;
	}

	if (!base[0] || !strcmp(base, "/")) {
		if (snprintf(buf, size, "%s/%s", rootfs, name) >= (int)size)
			return -ENAMETOOLONG;
		return 0;
	}

	if (snprintf(buf, size, "%s%s/%s", rootfs, base, name) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

static int flux_runc_exec_host_to_container_path(const char *rootfs,
						 const char *host_path,
						 char *buf, size_t size)
{
	size_t root_len;

	if (!rootfs || !rootfs[0] || !host_path || !host_path[0] || !buf ||
	    size == 0)
		return -EINVAL;

	root_len = strlen(rootfs);
	if (strncmp(host_path, rootfs, root_len) != 0)
		return -EINVAL;

	if (host_path[root_len] == '\0') {
		if (snprintf(buf, size, "/") >= (int)size)
			return -ENAMETOOLONG;
		return 0;
	}

	if (snprintf(buf, size, "%s", host_path + root_len) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

static int flux_runc_exec_resolve_filename(struct flux_runc_exec_spec *spec)
{
	const struct flux_oci_cfg *oci;
	const char *file;
	const char *cwd;
	const char *path_env;
	char candidate[PATH_MAX];
	char *paths = NULL;
	char *save = NULL;
	char *dir;
	int ret;

	if (!spec || !spec->argv || spec->argc <= 0 || !spec->argv[0])
		return -EINVAL;

	oci = flux_oci_cfg_get();
	if (!oci || !oci->rootfs_path || !oci->rootfs_path[0])
		return -EINVAL;

	file = spec->argv[0];
	cwd = spec->cwd && spec->cwd[0] ? spec->cwd : "/";

	if (file[0] == '/' || strchr(file, '/')) {
		ret = flux_runc_exec_build_candidate_path(oci->rootfs_path, cwd,
							  file, candidate,
							  sizeof(candidate));
		if (ret < 0)
			return ret;
		if (access(candidate, X_OK) < 0)
			return -errno;
		ret = flux_runc_exec_host_to_container_path(oci->rootfs_path,
							    candidate, candidate,
							    sizeof(candidate));
		if (ret < 0)
			return ret;
		return flux_runc_exec_strdup(&spec->filename, candidate);
	}

	path_env = flux_runc_exec_env_lookup(spec->env, spec->env_count, "PATH");
	if (!path_env || !path_env[0])
		path_env = "/bin:/usr/bin:/sbin:/usr/sbin";

	paths = strdup(path_env);
	if (!paths)
		return -ENOMEM;

	for (dir = strtok_r(paths, ":", &save); dir;
	     dir = strtok_r(NULL, ":", &save)) {
		ret = flux_runc_exec_build_candidate_path(oci->rootfs_path, dir,
							  file, candidate,
							  sizeof(candidate));
		if (ret < 0)
			continue;
		if (access(candidate, X_OK) == 0) {
			ret = flux_runc_exec_host_to_container_path(
				oci->rootfs_path, candidate, candidate,
				sizeof(candidate));
			if (ret < 0)
				continue;
			ret = flux_runc_exec_strdup(&spec->filename, candidate);
			free(paths);
			return ret;
		}
	}

	free(paths);
	return -ENOENT;
}

static int flux_runc_exec_apply_cli(const struct flux_runc_exec_cmd *cmd,
				    struct flux_runc_exec_spec *spec)
{
	int i;
	int ret;

	if (cmd->process_path) {
		ret = flux_runc_exec_parse_process_json(cmd->process_path,
							spec);
		if (ret < 0)
			return ret;
	} else {
		for (i = 0; i < cmd->argc; i++) {
			ret = flux_runc_exec_append_string(
				&spec->argv, &spec->argc, cmd->argv[i]);
			if (ret < 0)
				return ret;
		}
	}

	if (cmd->cwd) {
		ret = flux_runc_exec_strdup(&spec->cwd, cmd->cwd);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < cmd->env_count; i++) {
		ret = flux_runc_exec_replace_env(&spec->env, &spec->env_count,
						 cmd->env[i]);
		if (ret < 0)
			return ret;
	}

	ret = flux_runc_exec_parse_user(cmd->user_spec, spec);
	if (ret < 0)
		return ret;

	if (spec->argc <= 0 || !spec->argv || !spec->argv[0])
		return -EINVAL;

	if (!spec->cwd) {
		ret = flux_runc_exec_strdup(&spec->cwd, "/");
		if (ret < 0)
			return ret;
	}

	ret = flux_runc_exec_resolve_filename(spec);
	if (ret < 0)
		return ret;

	spec->detach = cmd->detach && !cmd->process_path;
	return 0;
}

static int flux_runc_exec_session_root(const struct flux_runc_state *state,
				       char *buf, size_t size)
{
	return flux_runc_state_exec_session_root(state, buf, size);
}

static int flux_runc_exec_mkdir(const char *path, mode_t mode)
{
	if (mkdir(path, mode) < 0 && errno != EEXIST)
		return -errno;

	return 0;
}

static int flux_runc_exec_session_path(const char *dir, const char *name,
				       char *buf, size_t size)
{
	if (snprintf(buf, size, "%s/%s", dir, name) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

static int flux_runc_exec_session_mkfifo(const char *dir, const char *name)
{
	char path[PATH_MAX];
	int ret;

	ret = flux_runc_exec_session_path(dir, name, path, sizeof(path));
	if (ret < 0)
		return ret;

	if (mkfifo(path, 0600) < 0)
		return -errno;

	return 0;
}

static int flux_runc_exec_session_symlink_fd(const char *dir, const char *name,
					     pid_t pid, int fdno)
{
	char path[PATH_MAX];
	char target[PATH_MAX];
	int ret;

	ret = flux_runc_exec_session_path(dir, name, path, sizeof(path));
	if (ret < 0)
		return ret;

	if (snprintf(target, sizeof(target), "/proc/%ld/fd/%d", (long)pid,
		     fdno) >= (int)sizeof(target))
		return -ENAMETOOLONG;

	if (symlink(target, path) < 0)
		return -errno;

	return 0;
}

static ssize_t flux_runc_exec_full_write(int fd, const void *buf, size_t len)
{
	const char *pos = buf;
	size_t left = len;

	while (left > 0) {
		ssize_t nw = write(fd, pos, left);

		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		pos += nw;
		left -= (size_t)nw;
	}

	return (ssize_t)len;
}

static void flux_runc_exec_session_bridge_or_exit(const char *path, int stdio_fd,
						  bool into_fifo)
	__attribute__((noreturn));

static void flux_runc_exec_session_bridge_or_exit(const char *path, int stdio_fd,
						  bool into_fifo)
{
	char buf[4096];
	int fifo_fd;

	fifo_fd = open(path, into_fifo ? O_WRONLY : O_RDONLY);
	if (fifo_fd < 0)
		_exit(127);

	for (;;) {
		ssize_t nr;

		nr = read(into_fifo ? stdio_fd : fifo_fd, buf, sizeof(buf));
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (nr == 0)
			break;
		if (flux_runc_exec_full_write(into_fifo ? fifo_fd : stdio_fd, buf,
					      (size_t)nr) < 0)
			break;
	}

	close(fifo_fd);
	_exit(0);
}

static int flux_runc_exec_session_spawn_bridge(const char *path, int stdio_fd,
					       bool into_fifo, pid_t *pid_out)
{
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -errno;
	if (pid == 0)
		flux_runc_exec_session_bridge_or_exit(path, stdio_fd, into_fifo);

	*pid_out = pid;
	return 0;
}

static void flux_runc_exec_session_reap_pid(pid_t *pid)
{
	int status;

	if (!pid || *pid <= 0)
		return;

	kill(*pid, SIGTERM);
	while (waitpid(*pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		break;
	}
	*pid = 0;
}

static void flux_runc_exec_session_destroy(struct flux_runc_exec_session *session)
{
	char path[PATH_MAX];
	static const char *names[] = {
		"stdin",
		"stdout",
		"stderr",
	};
	size_t i;

	if (!session || !session->active)
		return;

	flux_runc_exec_session_reap_pid(&session->stdin_pid);
	flux_runc_exec_session_reap_pid(&session->stdout_pid);
	flux_runc_exec_session_reap_pid(&session->stderr_pid);

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (flux_runc_exec_session_path(session->dir, names[i], path,
						sizeof(path)) < 0)
			continue;
		(void)unlink(path);
	}

	(void)rmdir(session->dir);
	memset(session, 0, sizeof(*session));
}

static uint32_t flux_runc_exec_session_base(const struct flux_runc_exec_spec *spec)
{
	/*
	 * Default attached exec uses stdio. A tty session is only requested
	 * explicitly through exec process metadata, e.g. process.json
	 * terminal=true.
	 */
	if (spec && spec->terminal)
		return FLUX_EXEC_SESSION_DYNAMIC_TTY_BASE;

	return FLUX_EXEC_SESSION_DYNAMIC_STDIO_BASE;
}

static uint32_t flux_runc_exec_session_nonce(void)
{
	struct timespec ts;
	uint32_t nonce;

	clock_gettime(CLOCK_REALTIME, &ts);
	nonce = (uint32_t)getpid() ^ (uint32_t)ts.tv_sec ^
		(uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_nsec << 11);
	nonce &= 0x0fffffffU;
	if (!nonce)
		nonce = 1;

	return nonce;
}

static int flux_runc_exec_session_create(const struct flux_runc_state *state,
					 struct flux_runc_exec_spec *spec,
					 struct flux_runc_exec_session *session)
{
	char root[PATH_MAX];
	char session_root[PATH_MAX];
	uint32_t base;
	uint32_t nonce;
	int ret;
	int attempt;

	if (!spec || !session)
		return -EINVAL;

	ret = flux_runc_exec_session_root(state, root, sizeof(root));
	if (ret < 0)
		return ret;

	ret = flux_runc_exec_mkdir(root, 0700);
	if (ret < 0)
		return ret;

	ret = flux_runc_exec_session_path(root, FLUX_RUNC_EXEC_SESSION_DIR,
					  session_root, sizeof(session_root));
	if (ret < 0)
		return ret;

	ret = flux_runc_exec_mkdir(session_root, 0700);
	if (ret < 0)
		return ret;

	base = flux_runc_exec_session_base(spec);
	nonce = flux_runc_exec_session_nonce();

	for (attempt = 0; attempt < 64; attempt++) {
		uint32_t id = base + ((nonce + (uint32_t)attempt) & 0x0fffffffU);

		if (snprintf(session->dir, sizeof(session->dir), "%s/%u",
			     session_root,
			     id) >= (int)sizeof(session->dir))
			return -ENAMETOOLONG;

		if (mkdir(session->dir, 0700) < 0) {
			if (errno == EEXIST)
				continue;
			return -errno;
		}

		session->id = id;
		session->active = true;
		spec->session_id = id;

		if (spec->terminal) {
			if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
			    !isatty(STDERR_FILENO)) {
				ret = -ENOTTY;
				goto err;
			}

			ret = flux_runc_exec_session_symlink_fd(session->dir,
								"stdin",
								getpid(), 0);
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_symlink_fd(session->dir,
								"stdout",
								getpid(), 1);
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_symlink_fd(session->dir,
								"stderr",
								getpid(), 2);
			if (ret < 0)
				goto err;
		} else {
			char path[PATH_MAX];

			ret = flux_runc_exec_session_mkfifo(session->dir, "stdin");
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_mkfifo(session->dir, "stdout");
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_mkfifo(session->dir, "stderr");
			if (ret < 0)
				goto err;

			ret = flux_runc_exec_session_path(session->dir, "stdin",
							  path, sizeof(path));
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_spawn_bridge(path,
							  STDIN_FILENO, true,
							  &session->stdin_pid);
			if (ret < 0)
				goto err;

			ret = flux_runc_exec_session_path(session->dir, "stdout",
							  path, sizeof(path));
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_spawn_bridge(
				path, STDOUT_FILENO, false,
				&session->stdout_pid);
			if (ret < 0)
				goto err;

			ret = flux_runc_exec_session_path(session->dir, "stderr",
							  path, sizeof(path));
			if (ret < 0)
				goto err;
			ret = flux_runc_exec_session_spawn_bridge(
				path, STDERR_FILENO, false,
				&session->stderr_pid);
			if (ret < 0)
				goto err;
		}

		return 0;
err:
		flux_runc_exec_session_destroy(session);
		return ret;
	}

	return -EEXIST;
}

static int flux_runc_exec_lock_open(const struct flux_runc_state *state)
{
	char path[PATH_MAX];
	int fd;

	if (snprintf(path, sizeof(path), "%s/exec.lock", state->state_dir) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;

	if (flock(fd, LOCK_EX) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	return fd;
}

static int flux_runc_exec_blob_append(char *data, size_t cap, size_t *off,
				      const void *src, size_t len,
				      uint32_t *offset_out)
{
	if (*off + len > cap)
		return -E2BIG;

	if (offset_out)
		*offset_out = (uint32_t)*off;
	if (src && len)
		memcpy(data + *off, src, len);
	else if (!src && len)
		memset(data + *off, 0, len);
	*off += len;
	return 0;
}

static int flux_runc_exec_serialize(struct flux_exec_slot *slot,
				    const struct flux_runc_exec_spec *spec)
{
	size_t off = 0;
	int i;
	int ret;

	ret = flux_runc_exec_blob_append((char *)slot->data, sizeof(slot->data),
					 &off, spec->filename,
					 strlen(spec->filename) + 1,
					 &slot->filename_off);
	if (ret < 0)
		return ret;

	if (spec->cwd) {
		slot->flags |= FLUX_EXEC_F_HAS_CWD;
		ret = flux_runc_exec_blob_append(
			(char *)slot->data, sizeof(slot->data), &off, spec->cwd,
			strlen(spec->cwd) + 1, &slot->cwd_off);
		if (ret < 0)
			return ret;
	}

	slot->argc = (uint32_t)spec->argc;
	ret = flux_runc_exec_blob_append((char *)slot->data, sizeof(slot->data),
					 &off, NULL,
					 sizeof(uint32_t) * (size_t)spec->argc,
					 &slot->argv_off);
	if (ret < 0)
		return ret;
	for (i = 0; i < spec->argc; i++) {
		uint32_t arg_off;

		ret = flux_runc_exec_blob_append(
			(char *)slot->data, sizeof(slot->data), &off,
			spec->argv[i], strlen(spec->argv[i]) + 1, &arg_off);
		if (ret < 0)
			return ret;
		((uint32_t *)(slot->data + slot->argv_off))[i] = arg_off;
	}

	if (spec->env_count > 0) {
		slot->flags |= FLUX_EXEC_F_HAS_ENV;
		slot->envc = (uint32_t)spec->env_count;
		ret = flux_runc_exec_blob_append(
			(char *)slot->data, sizeof(slot->data), &off, NULL,
			sizeof(uint32_t) * (size_t)spec->env_count,
			&slot->env_off);
		if (ret < 0)
			return ret;
		for (i = 0; i < spec->env_count; i++) {
			uint32_t env_off;

			ret = flux_runc_exec_blob_append(
				(char *)slot->data, sizeof(slot->data), &off,
				spec->env[i], strlen(spec->env[i]) + 1,
				&env_off);
			if (ret < 0)
				return ret;
			((uint32_t *)(slot->data + slot->env_off))[i] = env_off;
		}
	}

	if (spec->has_user) {
		slot->flags |= FLUX_EXEC_F_HAS_USER;
		slot->uid = (uint32_t)spec->uid;
		slot->gid = (uint32_t)spec->gid;
	}
	slot->session_id = spec->session_id;
	if (spec->detach)
		slot->flags |= FLUX_EXEC_F_DETACH;

	slot->data_len = (uint32_t)off;
	return 0;
}

static void flux_runc_exec_reset_slot(struct flux_exec_slot *slot)
{
	if (!slot)
		return;

	memset(slot, 0, sizeof(*slot));
	slot->state = FLUX_EXEC_SLOT_FREE;
}

static void flux_runc_exec_reap_head_slots(struct flux_exec_ring *ring)
{
	struct flux_exec_slot *slot;

	while (ring && !flux_exec_ring_is_empty(ring)) {
		slot = flux_exec_ring_slot(ring, ring->hdr.head);
		if (!slot)
			return;

		if (slot->state != FLUX_EXEC_SLOT_DONE &&
		    slot->state != FLUX_EXEC_SLOT_ERROR &&
		    slot->state != FLUX_EXEC_SLOT_FREE)
			return;

		flux_runc_exec_reset_slot(slot);
		ring->hdr.head =
			flux_exec_ring_next_index(ring, ring->hdr.head);
	}
}

static struct flux_exec_slot *
flux_runc_exec_reserve_slot(struct flux_exec_ring *ring, uint32_t *slot_idx)
{
	struct flux_exec_slot *slot;

	flux_runc_exec_reap_head_slots(ring);
	if (!ring || flux_exec_ring_is_full(ring))
		return NULL;

	*slot_idx = ring->hdr.tail;
	slot = flux_exec_ring_slot(ring, *slot_idx);
	if (!slot || slot->state != FLUX_EXEC_SLOT_FREE)
		return NULL;

	memset(slot, 0, sizeof(*slot));
	slot->seq = ring->hdr.next_seq++;
	slot->state = FLUX_EXEC_SLOT_FREE;
	return slot;
}

static int flux_runc_exec_publish_slot(struct flux_exec_ring *ring,
				       uint32_t slot_idx)
{
	struct flux_exec_slot *slot;

	slot = flux_exec_ring_slot(ring, slot_idx);
	if (!slot)
		return -EINVAL;

	flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_READY);
	flux_exec_ring_tail_store(ring,
				  flux_exec_ring_next_index(ring, slot_idx));
	return 0;
}

static void flux_runc_exec_unpublish_slot(struct flux_exec_ring *ring,
					  uint32_t slot_idx)
{
	if (!ring)
		return;

	if (flux_exec_ring_tail_load(ring) ==
	    flux_exec_ring_next_index(ring, slot_idx))
		flux_exec_ring_tail_store(ring, slot_idx);

	flux_runc_exec_reset_slot(flux_exec_ring_slot(ring, slot_idx));
	if (ring->hdr.head == slot_idx)
		flux_runc_exec_reap_head_slots(ring);
}

static int flux_runc_exec_wait_timeout_ms(void)
{
	const char *value;
	char *end = NULL;
	long parsed;

	value = getenv(FLUX_RUNC_EXEC_WAIT_TIMEOUT_ENV);
	if (!value || !value[0])
		return FLUX_RUNC_EXEC_WAIT_TIMEOUT_MS;

	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno != 0 || !end || *end != '\0' || parsed < 0)
		return FLUX_RUNC_EXEC_WAIT_TIMEOUT_MS;

	return parsed > INT_MAX ? INT_MAX : (int)parsed;
}

static int flux_runc_exec_wait_slot(struct flux_exec_ring *ring,
				    uint32_t slot_idx, uint64_t seq,
				    int timeout_ms, int *status_out)
{
	struct flux_exec_slot *slot;
	struct timespec req = {
		.tv_sec = 0,
		.tv_nsec = 10 * 1000 * 1000,
	};
	int waited_ms = 0;

	for (;;) {
		slot = flux_exec_ring_slot(ring, slot_idx);
		if (!slot || slot->seq != seq)
			return -EINVAL;
		if (flux_exec_slot_state_load(slot) == FLUX_EXEC_SLOT_DONE ||
		    flux_exec_slot_state_load(slot) == FLUX_EXEC_SLOT_ERROR) {
			*status_out = slot->status;
			memset(slot, 0, sizeof(*slot));
			flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_FREE);
			ring->hdr.head =
				flux_exec_ring_next_index(ring, slot_idx);
			return 0;
		}
		if (timeout_ms >= 0 && waited_ms >= timeout_ms)
			return -ETIMEDOUT;
		nanosleep(&req, NULL);
		waited_ms += 10;
	}
}

int flux_runc_cmd_exec(int argc, char **argv)
{
	struct flux_runc_exec_cmd cmd;
	struct flux_runc_exec_spec spec = { 0 };
	struct flux_runc_exec_session session = { 0 };
	struct flux_runc_state state;
	struct flux_exec_ring *ring = NULL;
	struct flux_exec_slot *slot;
	union sigval value;
	pid_t target_pid;
	int shm_fd = -1;
	int lock_fd = -1;
	int wait_timeout_ms;
	uint32_t slot_idx = 0;
	uint64_t seq = 0;
	int status = EXIT_FAILURE;
	bool cleanup_session = false;
	int ret;

	ret = flux_runc_parse_exec_command(argc, argv, &cmd);
	if (ret < 0)
		return EXIT_FAILURE;

	flux_runc_state_reset(&state);
	ret = flux_runc_state_load(&state, cmd.container_id);
	if (ret < 0) {
		flux_runc_log_errno("failed to load runtime state", ret);
		goto out;
	}

	ret = flux_runc_reconcile_loaded_state(
		&state, "failed to reconcile runtime state");
	if (ret < 0)
		goto out;
	if (state.status != FLUX_RUNC_RUNNING) {
		FLUX_LOG(FLUX_LOG_ERR, "container %s is not running\n",
			 state.id);
		ret = -EINVAL;
		goto out;
	}
	if (!state.exec_ring_name) {
		FLUX_LOG(FLUX_LOG_ERR, "container %s has no exec ring\n",
			 state.id);
		ret = -EINVAL;
		goto out;
	}

	ret = flux_runc_exec_load_oci_defaults(&state, &spec);
	if (ret < 0) {
		flux_runc_log_errno("failed to load OCI defaults", ret);
		goto out;
	}

	ret = flux_runc_exec_apply_cli(&cmd, &spec);
	if (ret < 0) {
		flux_runc_log_errno("failed to build exec request", ret);
		goto out;
	}

	ret = flux_runc_exec_session_create(&state, &spec, &session);
	if (ret < 0) {
		flux_runc_log_errno("failed to create exec session", ret);
		goto out;
	}
	cleanup_session = session.active;

	lock_fd = flux_runc_exec_lock_open(&state);
	if (lock_fd < 0) {
		ret = lock_fd;
		flux_runc_log_errno("failed to lock exec ring", ret);
		goto out;
	}

	ret = flux_runc_exec_ring_open(state.exec_ring_name, false, false,
				       &ring, &shm_fd);
	if (ret < 0) {
		flux_runc_log_errno("failed to map exec ring", ret);
		goto out;
	}
	wait_timeout_ms = flux_runc_exec_wait_timeout_ms();

	slot = flux_runc_exec_reserve_slot(ring, &slot_idx);
	if (!slot) {
		ret = -EBUSY;
		flux_runc_log_errno("failed to reserve exec ring slot", ret);
		goto out;
	}
	seq = slot->seq;

	ret = flux_runc_exec_serialize(slot, &spec);
	if (ret < 0) {
		flux_runc_log_errno("failed to serialize exec request", ret);
		goto out;
	}

	ret = flux_runc_exec_publish_slot(ring, slot_idx);
	if (ret < 0) {
		flux_runc_log_errno("failed to publish exec request", ret);
		goto out;
	}

	ret = flux_runc_state_control_pid(&state, &target_pid);
	if (ret < 0) {
		flux_runc_log_errno("failed to resolve exec control pid", ret);
		goto out;
	}

	value.sival_int =
		flux_signal_ctrl_pack(FLUX_SIGNAL_CTRL_EXEC, 0, slot_idx);
	if (sigqueue(target_pid, SIGUSR1, value) < 0) {
		ret = -errno;
		flux_runc_exec_unpublish_slot(ring, slot_idx);
		flux_runc_log_errno("failed to doorbell exec worker", ret);
		goto out;
	}

	if (cmd.pid_file) {
		ret = flux_runc_exec_write_pid_file(cmd.pid_file, getpid());
		if (ret < 0) {
			flux_runc_log_errno("failed to write exec pid file", ret);
			goto out;
		}
	}

	if (spec.detach) {
		cleanup_session = false;
		ret = 0;
		goto out;
	}

	ret = flux_runc_exec_wait_slot(ring, slot_idx, seq, wait_timeout_ms,
				       &status);
	if (ret < 0) {
		flux_runc_log_errno("failed waiting for exec result", ret);
		cleanup_session = false;
		goto out;
	}

	ret = status;
out:
	if (cleanup_session)
		flux_runc_exec_session_destroy(&session);
	flux_runc_unload();
	flux_runc_exec_ring_close(ring, shm_fd);
	if (lock_fd >= 0)
		close(lock_fd);
	flux_runc_exec_spec_fini(&spec);
	flux_runc_state_fini(&state);
	free(cmd.env);
	if (ret < 0)
		return EXIT_FAILURE;
	return ret;
}
