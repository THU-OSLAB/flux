#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <flux.h>
#include <flux/runc.h>
#include <kernel/linux/capability.h>

#define JSMN_HEADER
#include <utils/jsmn.h>

#define FLUX_OCI_TOKEN_MAX 4096

static struct flux_oci_cfg flux_oci_cfg;

static void flux_oci_free_string(char **value)
{
	if (!value || !*value)
		return;

	free(*value);
	*value = NULL;
}

static void flux_oci_free_string_array(char ***array, int *count)
{
	int i;

	if (!array || !*array)
		return;

	for (i = 0; i < *count; i++)
		free((*array)[i]);

	free(*array);
	*array = NULL;
	*count = 0;
}

static void flux_oci_free_mounts(void)
{
	int i;

	for (i = 0; i < flux_oci_cfg.mount_num; i++) {
		struct flux_oci_mount *mount = &flux_oci_cfg.mounts[i];

		free(mount->destination);
		free(mount->source);
		free(mount->type);
		free(mount->data);
	}

	free(flux_oci_cfg.mounts);
	flux_oci_cfg.mounts = NULL;
	flux_oci_cfg.mount_num = 0;
}

static void flux_oci_free_rlimits(void)
{
	free(flux_oci_cfg.rlimits);
	flux_oci_cfg.rlimits = NULL;
	flux_oci_cfg.rlimit_num = 0;
}

void flux_runc_unload(void)
{
	flux_oci_free_string(&flux_oci_cfg.bundle_dir);
	flux_oci_free_string(&flux_oci_cfg.config_path);
	flux_oci_free_string(&flux_oci_cfg.rootfs_path);
	flux_oci_free_string(&flux_oci_cfg.cwd);
	flux_oci_free_string(&flux_oci_cfg.hostname);
	flux_oci_free_string(&flux_oci_cfg.exec_path);
	flux_oci_free_string_array(&flux_oci_cfg.argv, &flux_oci_cfg.argc);
	flux_oci_free_string_array(&flux_oci_cfg.env, &flux_oci_cfg.env_num);
	flux_oci_free_rlimits();
	flux_oci_free_mounts();
	flux_oci_free_string_array(&flux_oci_cfg.masked_paths,
				   &flux_oci_cfg.masked_paths_num);
	flux_oci_free_string_array(&flux_oci_cfg.readonly_paths,
				   &flux_oci_cfg.readonly_paths_num);
	memset(&flux_oci_cfg, 0, sizeof(flux_oci_cfg));
}

static int flux_oci_strdup(char **dst, const char *src)
{
	char *copy;

	copy = strdup(src);
	if (!copy)
		return -1;

	free(*dst);
	*dst = copy;
	return 0;
}

static int flux_oci_token_str(const char *json, const jsmntok_t *tok,
			      char **out)
{
	int len;
	char *copy;

	if (!tok || !out)
		return -1;

	len = tok->end - tok->start;
	copy = malloc((size_t)len + 1);
	if (!copy)
		return -1;

	memcpy(copy, json + tok->start, (size_t)len);
	copy[len] = '\0';
	*out = copy;
	return 0;
}

static bool flux_oci_token_eq(const char *json, const jsmntok_t *tok,
			      const char *expect)
{
	size_t len;

	if (!tok || tok->type != JSMN_STRING)
		return false;

	len = strlen(expect);
	return (size_t)(tok->end - tok->start) == len &&
	       strncmp(json + tok->start, expect, len) == 0;
}

static int flux_oci_token_u64(const char *json, const jsmntok_t *tok,
			      unsigned long long *out)
{
	char buf[64];
	size_t len;
	char *end = NULL;
	unsigned long long value;

	if (!tok || tok->type != JSMN_PRIMITIVE || !out)
		return -1;

	len = (size_t)(tok->end - tok->start);
	if (len >= sizeof(buf))
		return -1;

	memcpy(buf, json + tok->start, len);
	buf[len] = '\0';

	errno = 0;
	value = strtoull(buf, &end, 10);
	if (errno || !end || *end != '\0')
		return -1;

	*out = value;
	return 0;
}

static int flux_oci_token_skip(const jsmntok_t *tokens, int index)
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
			index = flux_oci_token_skip(tokens, index);
		return index;
	case JSMN_OBJECT:
		size = tokens[index].size;
		index++;
		for (i = 0; i < size; i++) {
			index = flux_oci_token_skip(tokens, index);
			index = flux_oci_token_skip(tokens, index);
		}
		return index;
	default:
		return index + 1;
	}
}

static int flux_oci_parse_bool(const char *json, const jsmntok_t *tok,
			       bool *out)
{
	int len;

	if (!tok || tok->type != JSMN_PRIMITIVE)
		return -1;

	len = tok->end - tok->start;
	if (len == 4 && strncmp(json + tok->start, "true", 4) == 0) {
		*out = true;
		return 0;
	}
	if (len == 5 && strncmp(json + tok->start, "false", 5) == 0) {
		*out = false;
		return 0;
	}

	return -1;
}

static int flux_oci_append_string(char ***list, int *count, const char *value)
{
	char **new_list;
	char *copy;

	copy = strdup(value);
	if (!copy)
		return -1;

	new_list = realloc(*list, sizeof(*new_list) * (size_t)(*count + 1));
	if (!new_list) {
		free(copy);
		return -1;
	}

	new_list[*count] = copy;
	*list = new_list;
	(*count)++;
	return 0;
}

static int flux_oci_parse_string_array(const char *json,
				       const jsmntok_t *tokens, int index,
				       char ***out, int *count)
{
	int i;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		char *value = NULL;

		if (tokens[index + 1 + i].type != JSMN_STRING)
			return -1;

		if (flux_oci_token_str(json, &tokens[index + 1 + i], &value) < 0)
			return -1;

		if (flux_oci_append_string(out, count, value) < 0) {
			free(value);
			return -1;
		}

		free(value);
	}

	return 0;
}

struct flux_oci_cap_name_map {
	const char *name;
	unsigned int bit;
};

static const struct flux_oci_cap_name_map flux_oci_cap_names[] = {
	{ "CAP_CHOWN", FLUX_CAP_CHOWN },
	{ "CAP_DAC_OVERRIDE", FLUX_CAP_DAC_OVERRIDE },
	{ "CAP_DAC_READ_SEARCH", FLUX_CAP_DAC_READ_SEARCH },
	{ "CAP_FOWNER", FLUX_CAP_FOWNER },
	{ "CAP_FSETID", FLUX_CAP_FSETID },
	{ "CAP_KILL", FLUX_CAP_KILL },
	{ "CAP_SETGID", FLUX_CAP_SETGID },
	{ "CAP_SETUID", FLUX_CAP_SETUID },
	{ "CAP_SETPCAP", FLUX_CAP_SETPCAP },
	{ "CAP_LINUX_IMMUTABLE", FLUX_CAP_LINUX_IMMUTABLE },
	{ "CAP_NET_BIND_SERVICE", FLUX_CAP_NET_BIND_SERVICE },
	{ "CAP_NET_BROADCAST", FLUX_CAP_NET_BROADCAST },
	{ "CAP_NET_ADMIN", FLUX_CAP_NET_ADMIN },
	{ "CAP_NET_RAW", FLUX_CAP_NET_RAW },
	{ "CAP_IPC_LOCK", FLUX_CAP_IPC_LOCK },
	{ "CAP_IPC_OWNER", FLUX_CAP_IPC_OWNER },
	{ "CAP_SYS_MODULE", FLUX_CAP_SYS_MODULE },
	{ "CAP_SYS_RAWIO", FLUX_CAP_SYS_RAWIO },
	{ "CAP_SYS_CHROOT", FLUX_CAP_SYS_CHROOT },
	{ "CAP_SYS_PTRACE", FLUX_CAP_SYS_PTRACE },
	{ "CAP_SYS_PACCT", FLUX_CAP_SYS_PACCT },
	{ "CAP_SYS_ADMIN", FLUX_CAP_SYS_ADMIN },
	{ "CAP_SYS_BOOT", FLUX_CAP_SYS_BOOT },
	{ "CAP_SYS_NICE", FLUX_CAP_SYS_NICE },
	{ "CAP_SYS_RESOURCE", FLUX_CAP_SYS_RESOURCE },
	{ "CAP_SYS_TIME", FLUX_CAP_SYS_TIME },
	{ "CAP_SYS_TTY_CONFIG", FLUX_CAP_SYS_TTY_CONFIG },
	{ "CAP_MKNOD", FLUX_CAP_MKNOD },
	{ "CAP_LEASE", FLUX_CAP_LEASE },
	{ "CAP_AUDIT_WRITE", FLUX_CAP_AUDIT_WRITE },
	{ "CAP_AUDIT_CONTROL", FLUX_CAP_AUDIT_CONTROL },
	{ "CAP_SETFCAP", FLUX_CAP_SETFCAP },
	{ "CAP_MAC_OVERRIDE", FLUX_CAP_MAC_OVERRIDE },
	{ "CAP_MAC_ADMIN", FLUX_CAP_MAC_ADMIN },
	{ "CAP_SYSLOG", FLUX_CAP_SYSLOG },
	{ "CAP_WAKE_ALARM", FLUX_CAP_WAKE_ALARM },
	{ "CAP_BLOCK_SUSPEND", FLUX_CAP_BLOCK_SUSPEND },
	{ "CAP_AUDIT_READ", FLUX_CAP_AUDIT_READ },
	{ "CAP_PERFMON", FLUX_CAP_PERFMON },
	{ "CAP_BPF", FLUX_CAP_BPF },
	{ "CAP_CHECKPOINT_RESTORE", FLUX_CAP_CHECKPOINT_RESTORE },
};

struct flux_oci_rlimit_name_map {
	const char *name;
	unsigned int resource;
};

static const struct flux_oci_rlimit_name_map flux_oci_rlimit_names[] = {
	{ "RLIMIT_CPU", FLUX_RLIMIT_CPU },
	{ "RLIMIT_FSIZE", FLUX_RLIMIT_FSIZE },
	{ "RLIMIT_DATA", FLUX_RLIMIT_DATA },
	{ "RLIMIT_STACK", FLUX_RLIMIT_STACK },
	{ "RLIMIT_CORE", FLUX_RLIMIT_CORE },
	{ "RLIMIT_RSS", FLUX_RLIMIT_RSS },
	{ "RLIMIT_NPROC", FLUX_RLIMIT_NPROC },
	{ "RLIMIT_NOFILE", FLUX_RLIMIT_NOFILE },
	{ "RLIMIT_MEMLOCK", FLUX_RLIMIT_MEMLOCK },
	{ "RLIMIT_AS", FLUX_RLIMIT_AS },
	{ "RLIMIT_LOCKS", FLUX_RLIMIT_LOCKS },
	{ "RLIMIT_SIGPENDING", FLUX_RLIMIT_SIGPENDING },
	{ "RLIMIT_MSGQUEUE", FLUX_RLIMIT_MSGQUEUE },
	{ "RLIMIT_NICE", FLUX_RLIMIT_NICE },
	{ "RLIMIT_RTPRIO", FLUX_RLIMIT_RTPRIO },
	{ "RLIMIT_RTTIME", FLUX_RLIMIT_RTTIME },
};

static int flux_oci_cap_name_to_bit(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(flux_oci_cap_names) /
			       sizeof(flux_oci_cap_names[0]);
	     i++) {
		if (!strcmp(name, flux_oci_cap_names[i].name))
			return (int)flux_oci_cap_names[i].bit;
	}

	return -1;
}

static int flux_oci_rlimit_name_to_resource(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(flux_oci_rlimit_names) /
			       sizeof(flux_oci_rlimit_names[0]);
	     i++) {
		if (!strcmp(name, flux_oci_rlimit_names[i].name))
			return (int)flux_oci_rlimit_names[i].resource;
	}

	return -1;
}

static int flux_oci_parse_user(const char *json, const jsmntok_t *tokens,
			       int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;
		unsigned long long parsed;

		if (flux_oci_token_eq(json, &tokens[key], "uid")) {
			if (flux_oci_token_u64(json, &tokens[value], &parsed) < 0)
				return -1;
			flux_oci_cfg.user.uid = (uid_t)parsed;
			flux_oci_cfg.user.has_uid = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "gid")) {
			if (flux_oci_token_u64(json, &tokens[value], &parsed) < 0)
				return -1;
			flux_oci_cfg.user.gid = (gid_t)parsed;
			flux_oci_cfg.user.has_gid = true;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

static int flux_oci_parse_capability_array(const char *json,
					   const jsmntok_t *tokens, int index,
					   unsigned long long *mask)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		char *name = NULL;
		int bit;

		if (tokens[tok].type != JSMN_STRING)
			return -1;
		if (flux_oci_token_str(json, &tokens[tok], &name) < 0)
			return -1;

		bit = flux_oci_cap_name_to_bit(name);
		free(name);
		if (bit < 0)
			return -1;

		*mask |= 1ULL << (unsigned int)bit;
		tok = flux_oci_token_skip(tokens, tok);
	}

	return 0;
}

static int flux_oci_parse_capabilities(const char *json,
				       const jsmntok_t *tokens, int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "bounding")) {
			if (flux_oci_parse_capability_array(
				    json, tokens, value,
				    &flux_oci_cfg.capabilities.bounding) < 0)
				return -1;
			flux_oci_cfg.capabilities.has_bounding = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "effective")) {
			if (flux_oci_parse_capability_array(
				    json, tokens, value,
				    &flux_oci_cfg.capabilities.effective) < 0)
				return -1;
			flux_oci_cfg.capabilities.has_effective = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "permitted")) {
			if (flux_oci_parse_capability_array(
				    json, tokens, value,
				    &flux_oci_cfg.capabilities.permitted) < 0)
				return -1;
			flux_oci_cfg.capabilities.has_permitted = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "inheritable")) {
			if (flux_oci_parse_capability_array(
				    json, tokens, value,
				    &flux_oci_cfg.capabilities.inheritable) < 0)
				return -1;
			flux_oci_cfg.capabilities.has_inheritable = true;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

static int flux_oci_parse_rlimit(const char *json, const jsmntok_t *tokens,
				 int index, struct flux_oci_rlimit *rlimit)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;
		unsigned long long parsed;

		if (flux_oci_token_eq(json, &tokens[key], "type")) {
			char *name = NULL;
			int resource;

			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value], &name) < 0)
				return -1;

			resource = flux_oci_rlimit_name_to_resource(name);
			free(name);
			if (resource < 0)
				return -1;
			rlimit->resource = (unsigned int)resource;
		} else if (flux_oci_token_eq(json, &tokens[key], "soft")) {
			if (flux_oci_token_u64(json, &tokens[value], &parsed) < 0)
				return -1;
			rlimit->soft = (unsigned long)parsed;
			rlimit->has_soft = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "hard")) {
			if (flux_oci_token_u64(json, &tokens[value], &parsed) < 0)
				return -1;
			rlimit->hard = (unsigned long)parsed;
			rlimit->has_hard = true;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (!rlimit->has_soft || !rlimit->has_hard)
		return -1;

	return 0;
}

static int flux_oci_parse_rlimits(const char *json, const jsmntok_t *tokens,
				  int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	flux_oci_cfg.rlimits = calloc((size_t)tokens[index].size,
				      sizeof(*flux_oci_cfg.rlimits));
	if (!flux_oci_cfg.rlimits)
		return -1;

	flux_oci_cfg.rlimit_num = tokens[index].size;
	for (i = 0; i < flux_oci_cfg.rlimit_num; i++) {
		if (flux_oci_parse_rlimit(json, tokens, tok,
					  &flux_oci_cfg.rlimits[i]) < 0)
			return -1;
		tok = flux_oci_token_skip(tokens, tok);
	}

	return 0;
}

static int flux_oci_append_data_opt(char **data, const char *opt)
{
	size_t old_len = 0;
	size_t opt_len = strlen(opt);
	char *new_data;

	if (*data)
		old_len = strlen(*data);

	new_data = realloc(*data, old_len + (old_len ? 1 : 0) + opt_len + 1);
	if (!new_data)
		return -1;

	if (!old_len)
		new_data[0] = '\0';
	else
		new_data[old_len++] = ',';

	memcpy(new_data + old_len, opt, opt_len + 1);
	*data = new_data;
	return 0;
}

static bool flux_oci_mount_opt_flag(const char *opt, unsigned long *flags,
				    bool *use_hostfs)
{
	if (!strcmp(opt, "ro")) {
		*flags |= FLUX_MS_RDONLY;
		return true;
	}
	if (!strcmp(opt, "rw")) {
		*flags &= ~FLUX_MS_RDONLY;
		return true;
	}
	if (!strcmp(opt, "nosuid")) {
		*flags |= FLUX_MS_NOSUID;
		return true;
	}
	if (!strcmp(opt, "suid")) {
		*flags &= ~FLUX_MS_NOSUID;
		return true;
	}
	if (!strcmp(opt, "nodev")) {
		*flags |= FLUX_MS_NODEV;
		return true;
	}
	if (!strcmp(opt, "dev")) {
		*flags &= ~FLUX_MS_NODEV;
		return true;
	}
	if (!strcmp(opt, "noexec")) {
		*flags |= FLUX_MS_NOEXEC;
		return true;
	}
	if (!strcmp(opt, "exec")) {
		*flags &= ~FLUX_MS_NOEXEC;
		return true;
	}
	if (!strcmp(opt, "noatime")) {
		*flags |= FLUX_MS_NOATIME;
		return true;
	}
	if (!strcmp(opt, "nodiratime")) {
		*flags |= FLUX_MS_NODIRATIME;
		return true;
	}
	if (!strcmp(opt, "relatime")) {
		*flags |= FLUX_MS_RELATIME;
		return true;
	}
	if (!strcmp(opt, "strictatime")) {
		*flags |= FLUX_MS_STRICTATIME;
		return true;
	}
	if (!strcmp(opt, "dirsync")) {
		*flags |= FLUX_MS_DIRSYNC;
		return true;
	}
	if (!strcmp(opt, "sync")) {
		*flags |= FLUX_MS_SYNCHRONOUS;
		return true;
	}
	if (!strcmp(opt, "bind") || !strcmp(opt, "rbind")) {
		*use_hostfs = true;
		return true;
	}
	if (!strcmp(opt, "private") || !strcmp(opt, "rprivate") ||
	    !strcmp(opt, "slave") || !strcmp(opt, "rslave") ||
	    !strcmp(opt, "shared") || !strcmp(opt, "rshared") ||
	    !strcmp(opt, "unbindable") || !strcmp(opt, "runbindable") ||
	    !strcmp(opt, "rec") || !strcmp(opt, "defaults")) {
		return true;
	}

	return false;
}

static int flux_oci_parse_mount_options(const char *json,
					const jsmntok_t *tokens, int index,
					struct flux_oci_mount *mount)
{
	int i;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		char *opt = NULL;

		if (tokens[index + 1 + i].type != JSMN_STRING)
			return -1;

		if (flux_oci_token_str(json, &tokens[index + 1 + i], &opt) < 0)
			return -1;

		if (!flux_oci_mount_opt_flag(opt, &mount->flags,
					     &mount->use_hostfs) &&
		    flux_oci_append_data_opt(&mount->data, opt) < 0) {
			free(opt);
			return -1;
		}

		free(opt);
	}

	return 0;
}

static int flux_oci_host_realpath(const char *path, char **out)
{
	char resolved[PATH_MAX];

	if (!realpath(path, resolved)) {
		perror(path);
		return -1;
	}

	return flux_oci_strdup(out, resolved);
}

static int flux_oci_resolve_bundle_path(const char *base, const char *path,
					char **out)
{
	char full[PATH_MAX];

	if (path[0] == '/')
		return flux_oci_host_realpath(path, out);

	if (snprintf(full, sizeof(full), "%s/%s", base, path) >=
	    (int)sizeof(full))
		return -1;

	return flux_oci_host_realpath(full, out);
}

static int flux_oci_read_file(const char *path, char **buf_out)
{
	char *buf = NULL;
	ssize_t len;
	int fd = -1;
	int ret = -1;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		goto out;
	}

	len = lseek(fd, 0, SEEK_END);
	if (len < 0)
		goto out;

	if (lseek(fd, 0, SEEK_SET) < 0)
		goto out;

	buf = malloc((size_t)len + 1);
	if (!buf)
		goto out;

	if (read(fd, buf, (size_t)len) != len)
		goto out;

	buf[len] = '\0';
	*buf_out = buf;
	buf = NULL;
	ret = 0;
out:
	free(buf);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int flux_oci_resolve_container_existing(const char *container_path,
					       char **resolved_container_path)
{
	char full[PATH_MAX];

	if (!container_path || container_path[0] != '/')
		return -1;

	if (snprintf(full, sizeof(full), "%s%s", flux_oci_cfg.rootfs_path,
		     container_path) >= (int)sizeof(full))
		return -1;

	if (!realpath(full, NULL))
		return -1;

	return flux_oci_strdup(resolved_container_path, container_path);
}

static const char *flux_oci_cfg_get_env_value(const char *key)
{
	size_t key_len = strlen(key);
	int i;

	for (i = 0; i < flux_oci_cfg.env_num; i++) {
		const char *entry = flux_oci_cfg.env[i];

		if (strncmp(entry, key, key_len) == 0 && entry[key_len] == '=')
			return entry + key_len + 1;
	}

	return NULL;
}

static int flux_oci_build_candidate_path(const char *base, const char *name,
					 char **candidate)
{
	char path[PATH_MAX];

	if (base && base[0]) {
		if (snprintf(path, sizeof(path), "%s/%s", base, name) >=
		    (int)sizeof(path))
			return -1;
	} else {
		if (snprintf(path, sizeof(path), "/%s", name) >=
		    (int)sizeof(path))
			return -1;
	}

	return flux_oci_strdup(candidate, path);
}

static int flux_oci_resolve_exec_path(void)
{
	const char *path_env;
	const char *file;

	file = flux_oci_cfg.argv[0];
	if (file[0] == '/')
		return flux_oci_resolve_container_existing(
			file, &flux_oci_cfg.exec_path);

	if (strchr(file, '/')) {
		char *candidate = NULL;
		int ret;

		ret = flux_oci_build_candidate_path(
			flux_oci_cfg.cwd && strcmp(flux_oci_cfg.cwd, "/") ?
				flux_oci_cfg.cwd :
				"",
			file, &candidate);
		if (ret < 0)
			return ret;

		ret = flux_oci_resolve_container_existing(
			candidate, &flux_oci_cfg.exec_path);
		free(candidate);
		return ret;
	}

	path_env = flux_oci_cfg_get_env_value("PATH");
	if (path_env) {
		char *path_copy = strdup(path_env);
		char *save = NULL;
		char *dir;

		if (!path_copy)
			return -1;

		for (dir = strtok_r(path_copy, ":", &save); dir;
		     dir = strtok_r(NULL, ":", &save)) {
			char *candidate = NULL;

			if (dir[0] == '/') {
				if (flux_oci_build_candidate_path(dir, file,
								  &candidate) <
				    0)
					continue;
			} else if (flux_oci_build_candidate_path(
					   flux_oci_cfg.cwd &&
						   strcmp(flux_oci_cfg.cwd,
							  "/") ?
						   flux_oci_cfg.cwd :
						   "",
					   file, &candidate) < 0) {
				continue;
			}

			if (candidate &&
			    flux_oci_resolve_container_existing(
				    candidate, &flux_oci_cfg.exec_path) == 0) {
				free(candidate);
				free(path_copy);
				return 0;
			}

			free(candidate);
		}

		free(path_copy);
	}

	return -1;
}

static int flux_oci_parse_root(const char *json, const jsmntok_t *tokens,
			       int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "path")) {
			char *root = NULL;

			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value], &root) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_root failed on path token type=%d\n",
					 tokens[value].type);
				return -1;
			}

			if (flux_oci_resolve_bundle_path(
				    flux_oci_cfg.bundle_dir, root,
				    &flux_oci_cfg.rootfs_path) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_root failed to resolve path %s\n",
					 root);
				free(root);
				return -1;
			}

			free(root);
		} else if (flux_oci_token_eq(json, &tokens[key], "readonly")) {
			if (flux_oci_parse_bool(json, &tokens[value],
						&flux_oci_cfg.rootfs_readonly) <
			    0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_root failed on readonly\n");
				return -1;
			}
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

static int flux_oci_parse_process(const char *json, const jsmntok_t *tokens,
				  int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "args")) {
			if (flux_oci_parse_string_array(json, tokens, value,
							&flux_oci_cfg.argv,
							&flux_oci_cfg.argc) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on args\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "env")) {
			if (flux_oci_parse_string_array(json, tokens, value,
							&flux_oci_cfg.env,
							&flux_oci_cfg.env_num) <
			    0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on env\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "cwd")) {
			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value],
					       &flux_oci_cfg.cwd) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on cwd token type=%d\n",
					 tokens[value].type);
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "user")) {
			if (flux_oci_parse_user(json, tokens, value) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on user\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key],
					   "capabilities")) {
			if (flux_oci_parse_capabilities(json, tokens, value) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on capabilities\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "rlimits")) {
			if (flux_oci_parse_rlimits(json, tokens, value) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on rlimits\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key],
					   "noNewPrivileges")) {
			if (flux_oci_parse_bool(json, &tokens[value],
						&flux_oci_cfg.no_new_privileges) <
			    0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on noNewPrivileges\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "terminal")) {
			if (flux_oci_parse_bool(json, &tokens[value],
						&flux_oci_cfg.terminal) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on terminal\n");
				return -1;
			}
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

static int flux_oci_parse_mount(const char *json, const jsmntok_t *tokens,
				int index, struct flux_oci_mount *mount)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "destination")) {
			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value],
					       &mount->destination) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "source")) {
			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value],
					       &mount->source) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "type")) {
			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value],
					       &mount->type) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "options")) {
			if (flux_oci_parse_mount_options(json, tokens, value,
							 mount) < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (!mount->destination || mount->destination[0] != '/')
		return -1;

	if (!mount->type)
		return -1;

	return 0;
}

static int flux_oci_parse_mounts(const char *json, const jsmntok_t *tokens,
				 int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	flux_oci_cfg.mounts =
		calloc((size_t)tokens[index].size, sizeof(*flux_oci_cfg.mounts));
	if (!flux_oci_cfg.mounts)
		return -1;

	flux_oci_cfg.mount_num = tokens[index].size;
	for (i = 0; i < flux_oci_cfg.mount_num; i++) {
		if (flux_oci_parse_mount(json, tokens, tok,
					 &flux_oci_cfg.mounts[i]) < 0)
			return -1;
		tok = flux_oci_token_skip(tokens, tok);
	}

	return 0;
}

static int flux_oci_parse_linux(const char *json, const jsmntok_t *tokens,
				int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "maskedPaths")) {
			if (flux_oci_parse_string_array(json, tokens, value,
							&flux_oci_cfg.masked_paths,
							&flux_oci_cfg.masked_paths_num) <
			    0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "readonlyPaths")) {
			if (flux_oci_parse_string_array(
				    json, tokens, value,
				    &flux_oci_cfg.readonly_paths,
				    &flux_oci_cfg.readonly_paths_num) < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

static int flux_oci_validate_mount(struct flux_oci_mount *mount)
{
	char *source = NULL;

	if (!mount->destination || mount->destination[0] != '/')
		return -1;

	if (!mount->type)
		return -1;

	if (!mount->use_hostfs)
		return 0;

	if (!mount->source)
		return -1;

	if (flux_oci_resolve_bundle_path(flux_oci_cfg.bundle_dir, mount->source,
					 &source) < 0)
		return -1;

	free(mount->source);
	mount->source = source;
	return 0;
}

static int flux_oci_validate_process(void)
{
	if (!flux_oci_cfg.argv || flux_oci_cfg.argc <= 0)
		return -1;

	if (!flux_oci_cfg.cwd || flux_oci_cfg.cwd[0] != '/')
		return -1;

	return 0;
}

static int flux_oci_validate_linux_path_list(char *const *paths, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (!paths[i] || paths[i][0] != '/')
			return -1;
	}

	return 0;
}

static int flux_oci_validate_and_normalize(void)
{
	int i;

	if (!flux_oci_cfg.rootfs_path)
		return -1;

	if (flux_oci_validate_process() < 0)
		return -1;

	for (i = 0; i < flux_oci_cfg.mount_num; i++) {
		if (flux_oci_validate_mount(&flux_oci_cfg.mounts[i]) < 0)
			return -1;
	}

	if (flux_oci_validate_linux_path_list(flux_oci_cfg.masked_paths,
					      flux_oci_cfg.masked_paths_num) < 0)
		return -1;
	if (flux_oci_validate_linux_path_list(flux_oci_cfg.readonly_paths,
					      flux_oci_cfg.readonly_paths_num) < 0)
		return -1;

	if (flux_oci_resolve_exec_path() < 0)
		return -1;

	return 0;
}

static int flux_oci_parse_config(const char *json)
{
	jsmn_parser parser;
	jsmntok_t tokens[FLUX_OCI_TOKEN_MAX];
	int i;
	int tok;
	int ret;

	jsmn_init(&parser);
	ret = jsmn_parse(&parser, json, strlen(json), tokens,
			 FLUX_OCI_TOKEN_MAX);
	if (ret < 0)
		return ret;

	if (ret < 1 || tokens[0].type != JSMN_OBJECT)
		return -1;

	tok = 1;
	for (i = 0; i < tokens[0].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "root")) {
			if (flux_oci_parse_root(json, tokens, value) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "process")) {
			if (flux_oci_parse_process(json, tokens, value) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "hostname")) {
			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value],
					       &flux_oci_cfg.hostname) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "mounts")) {
			if (flux_oci_parse_mounts(json, tokens, value) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "linux")) {
			if (flux_oci_parse_linux(json, tokens, value) < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (flux_oci_validate_and_normalize() < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "invalid or unsupported OCI config\n");
		return -1;
	}

	return 0;
}

int flux_runc_set_bundle(const char *bundle_dir)
{
	char config_path[PATH_MAX];

	if (snprintf(config_path, sizeof(config_path), "%s/config.json",
		     bundle_dir) >= (int)sizeof(config_path))
		return -1;

	return flux_runc_set_bundle_config(bundle_dir, config_path);
}

int flux_runc_set_bundle_config(const char *bundle_dir, const char *config_path)
{
	flux_runc_unload();

	if (flux_oci_host_realpath(bundle_dir, &flux_oci_cfg.bundle_dir) < 0)
		return -1;

	if (flux_oci_host_realpath(config_path, &flux_oci_cfg.config_path) < 0)
		return -1;

	return 0;
}

int flux_runc_load_bundle(void)
{
	char *json = NULL;
	int ret;

	if (!flux_oci_cfg.config_path)
		return -1;

	ret = flux_oci_read_file(flux_oci_cfg.config_path, &json);
	if (ret < 0)
		return ret;

	ret = flux_oci_parse_config(json);
	free(json);
	return ret;
}

bool flux_runc_bundle_enabled(void)
{
	return flux_oci_cfg.bundle_dir != NULL;
}

const struct flux_oci_cfg *flux_oci_cfg_get(void)
{
	return flux_runc_bundle_enabled() ? &flux_oci_cfg : NULL;
}
