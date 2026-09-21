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
#define FLUX_OCI_VERSION_MAJOR 1U
#define FLUX_OCI_VERSION_MAX_MINOR 3U
#define FLUX_OCI_VERSION_MAX_PATCH 0U

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

static void flux_oci_free_sysctls(void)
{
	int i;

	for (i = 0; i < flux_oci_cfg.sysctl_num; i++) {
		free(flux_oci_cfg.sysctls[i].name);
		free(flux_oci_cfg.sysctls[i].value);
	}
	free(flux_oci_cfg.sysctls);
	flux_oci_cfg.sysctls = NULL;
	flux_oci_cfg.sysctl_num = 0;
}

void flux_oci_resources_fini(struct flux_oci_resources *resources)
{
	int i;

	if (!resources)
		return;

	free(resources->cpu.cpus);
	free(resources->cpu.mems);
	free(resources->block_io.weight_devices);
	free(resources->block_io.throttle_read_bps);
	free(resources->block_io.throttle_write_bps);
	free(resources->block_io.throttle_read_iops);
	free(resources->block_io.throttle_write_iops);
	for (i = 0; i < resources->network.priority_num; i++)
		free(resources->network.priorities[i].name);
	free(resources->network.priorities);
	memset(resources, 0, sizeof(*resources));
}

void flux_runc_unload(void)
{
	flux_oci_free_string(&flux_oci_cfg.oci_version);
	flux_oci_free_string(&flux_oci_cfg.bundle_dir);
	flux_oci_free_string(&flux_oci_cfg.config_path);
	flux_oci_free_string(&flux_oci_cfg.rootfs_path);
	flux_oci_free_string(&flux_oci_cfg.cwd);
	flux_oci_free_string(&flux_oci_cfg.hostname);
	flux_oci_free_string(&flux_oci_cfg.exec_path);
	flux_oci_free_string_array(&flux_oci_cfg.argv, &flux_oci_cfg.argc);
	flux_oci_free_string_array(&flux_oci_cfg.env, &flux_oci_cfg.env_num);
	free(flux_oci_cfg.user.additional_gids);
	flux_oci_cfg.user.additional_gids = NULL;
	flux_oci_cfg.user.additional_gid_num = 0;
	flux_oci_free_rlimits();
	free(flux_oci_cfg.device_rules);
	flux_oci_cfg.device_rules = NULL;
	flux_oci_cfg.device_rule_num = 0;
	flux_oci_resources_fini(&flux_oci_cfg.resources);
	flux_oci_free_sysctls();
	flux_oci_free_mounts();
	flux_oci_free_string_array(&flux_oci_cfg.masked_paths,
				   &flux_oci_cfg.masked_paths_num);
	flux_oci_free_string_array(&flux_oci_cfg.readonly_paths,
				   &flux_oci_cfg.readonly_paths_num);
	flux_oci_free_string(&flux_oci_cfg.cgroups_path);
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

static int flux_oci_hex_digit(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

static int flux_oci_parse_hex4(const char *value, unsigned int *codepoint)
{
	unsigned int result = 0;
	int i;

	for (i = 0; i < 4; i++) {
		int digit = flux_oci_hex_digit(value[i]);

		if (digit < 0)
			return -1;
		result = (result << 4) | (unsigned int)digit;
	}
	*codepoint = result;
	return 0;
}

static int flux_oci_append_utf8(char *dst, size_t capacity, size_t *offset,
				unsigned int codepoint)
{
	if (codepoint == 0 || codepoint > 0x10ffff ||
	    (codepoint >= 0xd800 && codepoint <= 0xdfff))
		return -1;
	if (codepoint <= 0x7f) {
		if (*offset + 1 >= capacity)
			return -1;
		dst[(*offset)++] = (char)codepoint;
	} else if (codepoint <= 0x7ff) {
		if (*offset + 2 >= capacity)
			return -1;
		dst[(*offset)++] = (char)(0xc0 | (codepoint >> 6));
		dst[(*offset)++] = (char)(0x80 | (codepoint & 0x3f));
	} else if (codepoint <= 0xffff) {
		if (*offset + 3 >= capacity)
			return -1;
		dst[(*offset)++] = (char)(0xe0 | (codepoint >> 12));
		dst[(*offset)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
		dst[(*offset)++] = (char)(0x80 | (codepoint & 0x3f));
	} else {
		if (*offset + 4 >= capacity)
			return -1;
		dst[(*offset)++] = (char)(0xf0 | (codepoint >> 18));
		dst[(*offset)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
		dst[(*offset)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
		dst[(*offset)++] = (char)(0x80 | (codepoint & 0x3f));
	}
	return 0;
}

static int flux_oci_token_str(const char *json, const jsmntok_t *tok,
			      char **out)
{
	size_t in;
	size_t len;
	size_t offset = 0;
	char *copy;

	if (!tok || !out)
		return -1;

	len = (size_t)(tok->end - tok->start);
	copy = malloc(len + 1);
	if (!copy)
		return -1;

	for (in = 0; in < len; in++) {
		char ch = json[tok->start + (int)in];

		if (ch != '\\') {
			copy[offset++] = ch;
			continue;
		}
		if (++in >= len)
			goto invalid;
		ch = json[tok->start + (int)in];
		switch (ch) {
		case '"':
		case '\\':
		case '/':
			copy[offset++] = ch;
			break;
		case 'b':
			copy[offset++] = '\b';
			break;
		case 'f':
			copy[offset++] = '\f';
			break;
		case 'n':
			copy[offset++] = '\n';
			break;
		case 'r':
			copy[offset++] = '\r';
			break;
		case 't':
			copy[offset++] = '\t';
			break;
		case 'u': {
			unsigned int codepoint;

			if (in + 4 >= len ||
			    flux_oci_parse_hex4(json + tok->start + (int)in + 1,
						&codepoint) < 0)
				goto invalid;
			in += 4;
			if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
				unsigned int low;

				if (in + 6 >= len ||
				    json[tok->start + (int)in + 1] != '\\' ||
				    json[tok->start + (int)in + 2] != 'u' ||
				    flux_oci_parse_hex4(
					    json + tok->start + (int)in + 3,
					    &low) < 0 ||
				    low < 0xdc00 || low > 0xdfff)
					goto invalid;
				codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
					    (low - 0xdc00);
				in += 6;
			}
			if (flux_oci_append_utf8(copy, len + 1, &offset,
						 codepoint) < 0)
				goto invalid;
			break;
		}
		default:
			goto invalid;
		}
	}
	copy[offset] = '\0';
	*out = copy;
	return 0;

invalid:
	free(copy);
	return -1;
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

static int flux_oci_token_i64(const char *json, const jsmntok_t *tok,
			      long long *out)
{
	char buf[64];
	size_t len;
	char *end = NULL;
	long long value;

	if (!tok || tok->type != JSMN_PRIMITIVE || !out)
		return -1;

	len = (size_t)(tok->end - tok->start);
	if (len >= sizeof(buf))
		return -1;

	memcpy(buf, json + tok->start, len);
	buf[len] = '\0';

	errno = 0;
	value = strtoll(buf, &end, 10);
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

static bool flux_oci_token_is_empty(const char *json,
				    const jsmntok_t *tok)
{
	if (!tok)
		return false;

	if (tok->type == JSMN_ARRAY || tok->type == JSMN_OBJECT)
		return tok->size == 0;
	if (tok->type == JSMN_STRING)
		return tok->start == tok->end;
	if (tok->type == JSMN_PRIMITIVE && tok->end - tok->start == 4)
		return strncmp(json + tok->start, "null", 4) == 0;

	return false;
}

static int flux_oci_reject_nonempty(const char *json, const jsmntok_t *tok,
				    const char *field)
{
	if (flux_oci_token_is_empty(json, tok))
		return 0;

	FLUX_LOG(FLUX_LOG_ERR, "unsupported OCI field: %s\n", field);
	return -1;
}

static int flux_oci_parse_version(const char *json, const jsmntok_t *tok)
{
	unsigned int major;
	unsigned int minor;
	unsigned int patch;
	char *version = NULL;
	char trailing;
	int parsed;

	if (!tok || tok->type != JSMN_STRING ||
	    flux_oci_token_str(json, tok, &version) < 0)
		return -1;

	parsed = sscanf(version, "%u.%u.%u%c", &major, &minor, &patch,
			&trailing);
	if (parsed < 3 ||
	    (parsed == 4 && trailing != '-' && trailing != '+') ||
	    major != FLUX_OCI_VERSION_MAJOR ||
	    minor > FLUX_OCI_VERSION_MAX_MINOR ||
	    (minor == FLUX_OCI_VERSION_MAX_MINOR &&
	     patch > FLUX_OCI_VERSION_MAX_PATCH)) {
		FLUX_LOG(FLUX_LOG_ERR, "unsupported OCI version: %s\n", version);
		free(version);
		return -1;
	}

	free(flux_oci_cfg.oci_version);
	flux_oci_cfg.oci_version = version;
	return 0;
}

static int flux_oci_append_string(char ***list, int *count, const char *value)
{
	char **new_list;
	char *copy;

	copy = strdup(value);
	if (!copy)
		return -1;

	/* kernel_execve() consumes argv/envp as NULL-terminated vectors.  Keep
	 * that invariant after every append instead of relying on allocator
	 * contents beyond the last element. */
	new_list = realloc(*list, sizeof(*new_list) * (size_t)(*count + 2));
	if (!new_list) {
		free(copy);
		return -1;
	}

	new_list[*count] = copy;
	new_list[*count + 1] = NULL;
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
		} else if (flux_oci_token_eq(json, &tokens[key],
					   "additionalGids")) {
			int j;
			int gid_tok = value + 1;

			if (tokens[value].type != JSMN_ARRAY)
				return -1;
			free(flux_oci_cfg.user.additional_gids);
			flux_oci_cfg.user.additional_gids = NULL;
			flux_oci_cfg.user.additional_gid_num = 0;
			if (tokens[value].size > 0) {
				flux_oci_cfg.user.additional_gids = calloc(
					(size_t)tokens[value].size,
					sizeof(*flux_oci_cfg.user.additional_gids));
				if (!flux_oci_cfg.user.additional_gids)
					return -1;
			}
			for (j = 0; j < tokens[value].size; j++) {
				gid_t gid;

				if (flux_oci_token_u64(json, &tokens[gid_tok],
						       &parsed) < 0)
					return -1;
				gid = (gid_t)parsed;
				if ((unsigned long long)gid != parsed)
					return -1;
				flux_oci_cfg.user.additional_gids[j] = gid;
				flux_oci_cfg.user.additional_gid_num++;
				gid_tok = flux_oci_token_skip(tokens, gid_tok);
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "username")) {
			if (flux_oci_reject_nonempty(
				    json, &tokens[value],
				    "process.user.username") < 0)
				return -1;
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
		} else if (flux_oci_token_eq(json, &tokens[key], "ambient")) {
			if (flux_oci_reject_nonempty(
				    json, &tokens[value],
				    "process.capabilities.ambient") < 0)
				return -1;
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

static int flux_oci_parse_console_size(const char *json,
				       const jsmntok_t *tokens, int index)
{
	bool width_seen = false;
	bool height_seen = false;
	int tok = index + 1;
	int i;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;
		unsigned long long parsed;

		if (flux_oci_token_eq(json, &tokens[key], "width")) {
			if (flux_oci_token_u64(json, &tokens[value], &parsed) < 0 ||
			    parsed > USHRT_MAX)
				return -1;
			flux_oci_cfg.console_width = (unsigned int)parsed;
			width_seen = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "height")) {
			if (flux_oci_token_u64(json, &tokens[value], &parsed) < 0 ||
			    parsed > USHRT_MAX)
				return -1;
			flux_oci_cfg.console_height = (unsigned int)parsed;
			height_seen = true;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (!width_seen || !height_seen)
		return -1;
	flux_oci_cfg.has_console_size = true;
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
		} else if (flux_oci_token_eq(json, &tokens[key],
					   "apparmorProfile")) {
			char *profile = NULL;

			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value], &profile) < 0)
				return -1;
			if (profile[0] && strcmp(profile, "unconfined")) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "unsupported OCI field: process.apparmorProfile=%s\n",
					 profile);
				free(profile);
				return -1;
			}
			free(profile);
		} else if (flux_oci_token_eq(json, &tokens[key], "selinuxLabel")) {
			if (flux_oci_reject_nonempty(
				    json, &tokens[value],
				    "process.selinuxLabel") < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "oomScoreAdj")) {
			long long adjustment;

			if (flux_oci_token_i64(json, &tokens[value], &adjustment) < 0)
				return -1;
			if (adjustment != 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "unsupported OCI field: process.oomScoreAdj=%lld\n",
					 adjustment);
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "consoleSize")) {
			if (flux_oci_parse_console_size(json, tokens, value) < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI parse_process failed on consoleSize\n");
				return -1;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "scheduler") ||
			   flux_oci_token_eq(json, &tokens[key], "ioPriority") ||
			   flux_oci_token_eq(json, &tokens[key], "execCPUAffinity") ||
			   flux_oci_token_eq(json, &tokens[key], "umask")) {
			char *field = NULL;

			if (flux_oci_token_str(json, &tokens[key], &field) < 0)
				return -1;
			FLUX_LOG(FLUX_LOG_ERR, "unsupported OCI field: process.%s\n",
				 field);
			free(field);
			return -1;
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

struct flux_oci_namespace_name_map {
	const char *name;
	unsigned int flag;
};

static const struct flux_oci_namespace_name_map flux_oci_namespace_names[] = {
	{ "pid", FLUX_OCI_NS_PID },
	{ "ipc", FLUX_OCI_NS_IPC },
	{ "uts", FLUX_OCI_NS_UTS },
	{ "mount", FLUX_OCI_NS_MOUNT },
	{ "network", FLUX_OCI_NS_NETWORK },
	{ "cgroup", FLUX_OCI_NS_CGROUP },
	{ "time", FLUX_OCI_NS_TIME },
};

static int flux_oci_namespace_flag(const char *name, unsigned int *flag)
{
	size_t i;

	for (i = 0; i < sizeof(flux_oci_namespace_names) /
			       sizeof(flux_oci_namespace_names[0]);
	     i++) {
		if (!strcmp(name, flux_oci_namespace_names[i].name)) {
			*flag = flux_oci_namespace_names[i].flag;
			return 0;
		}
	}

	return -1;
}

static int flux_oci_parse_namespace(const char *json,
				    const jsmntok_t *tokens, int index)
{
	char *type = NULL;
	unsigned int flag;
	int i;
	int tok = index + 1;
	int ret = -1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "type")) {
			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value], &type) < 0)
				goto out;
		} else if (flux_oci_token_eq(json, &tokens[key], "path")) {
			if (flux_oci_reject_nonempty(
				    json, &tokens[value],
				    "linux.namespaces[].path") < 0)
				goto out;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (!type) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "missing required OCI field: linux.namespaces[].type\n");
		goto out;
	}

	/* Each Flux instance already owns its LibOS PID, IPC, UTS, mount,
	 * network, cgroup and clock views. Joining host namespaces, user-id
	 * mappings and nonzero time offsets need different mechanisms. */
	if (flux_oci_namespace_flag(type, &flag) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "unsupported OCI namespace type: %s\n", type);
		goto out;
	}
	if (flux_oci_cfg.namespace_flags & flag) {
		FLUX_LOG(FLUX_LOG_ERR, "duplicate OCI namespace type: %s\n",
			 type);
		goto out;
	}

	flux_oci_cfg.namespace_flags |= flag;
	ret = 0;
out:
	free(type);
	return ret;
}

static int flux_oci_parse_namespaces(const char *json,
				     const jsmntok_t *tokens, int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		if (flux_oci_parse_namespace(json, tokens, tok) < 0)
			return -1;
		tok = flux_oci_token_skip(tokens, tok);
	}

	return 0;
}

static int flux_oci_parse_device_access(const char *json,
					const jsmntok_t *tok,
					unsigned int *access)
{
	char *value = NULL;
	const char *p;

	if (!tok || tok->type != JSMN_STRING ||
	    flux_oci_token_str(json, tok, &value) < 0)
		return -1;
	if (!value[0]) {
		free(value);
		return -1;
	}

	*access = 0;
	for (p = value; *p; p++) {
		switch (*p) {
		case 'r':
			*access |= FLUX_OCI_DEVICE_READ;
			break;
		case 'w':
			*access |= FLUX_OCI_DEVICE_WRITE;
			break;
		case 'm':
			*access |= FLUX_OCI_DEVICE_MKNOD;
			break;
		default:
			free(value);
			return -1;
		}
	}

	free(value);
	return 0;
}

static int flux_oci_parse_device_rule(const char *json,
				      const jsmntok_t *tokens, int index,
				      struct flux_oci_device_rule *rule)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "allow")) {
			if (flux_oci_parse_bool(json, &tokens[value],
						&rule->allow) < 0)
				return -1;
			rule->has_allow = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "type")) {
			char *type = NULL;

			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value], &type) < 0)
				return -1;
			if (strlen(type) != 1 ||
			    (type[0] != 'a' && type[0] != 'b' && type[0] != 'c')) {
				free(type);
				return -1;
			}
			rule->type = type[0];
			free(type);
		} else if (flux_oci_token_eq(json, &tokens[key], "major") ||
			   flux_oci_token_eq(json, &tokens[key], "minor")) {
			long long number;
			long long *dst;
			bool *has;

			if (flux_oci_token_is_empty(json, &tokens[value])) {
				tok = flux_oci_token_skip(tokens, value);
				continue;
			}
			if (flux_oci_token_i64(json, &tokens[value], &number) < 0 ||
			    number < -1)
				return -1;
			dst = flux_oci_token_eq(json, &tokens[key], "major") ?
				      &rule->major : &rule->minor;
			has = flux_oci_token_eq(json, &tokens[key], "major") ?
				      &rule->has_major : &rule->has_minor;
			*dst = number;
			*has = number >= 0;
		} else if (flux_oci_token_eq(json, &tokens[key], "access")) {
			if (flux_oci_parse_device_access(
				    json, &tokens[value], &rule->access) < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (!rule->has_allow || !rule->access)
		return -1;

	return 0;
}

static int flux_oci_parse_device_rules(const char *json,
				       const jsmntok_t *tokens, int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;

	free(flux_oci_cfg.device_rules);
	flux_oci_cfg.device_rules = NULL;
	flux_oci_cfg.device_rule_num = 0;
	if (tokens[index].size > 0) {
		flux_oci_cfg.device_rules = calloc(
			(size_t)tokens[index].size,
			sizeof(*flux_oci_cfg.device_rules));
		if (!flux_oci_cfg.device_rules)
			return -1;
	}

	for (i = 0; i < tokens[index].size; i++) {
		if (flux_oci_parse_device_rule(
			    json, tokens, tok, &flux_oci_cfg.device_rules[i]) < 0)
			return -1;
		flux_oci_cfg.device_rule_num++;
		tok = flux_oci_token_skip(tokens, tok);
	}

	return 0;
}

bool flux_oci_device_allowed(const struct flux_oci_cfg *oci, char type,
			     unsigned int major, unsigned int minor,
			     unsigned int access)
{
	unsigned int bit;
	int i;

	if (!oci || oci->device_rule_num == 0)
		return true;

	for (bit = FLUX_OCI_DEVICE_READ; bit <= FLUX_OCI_DEVICE_MKNOD;
	     bit <<= 1) {
		bool allowed = true;

		if (!(access & bit))
			continue;
		for (i = 0; i < oci->device_rule_num; i++) {
			const struct flux_oci_device_rule *rule =
				&oci->device_rules[i];

			if (rule->type && rule->type != 'a' &&
			    rule->type != type)
				continue;
			if (rule->has_major && rule->major != (long long)major)
				continue;
			if (rule->has_minor && rule->minor != (long long)minor)
				continue;
			if (rule->access & bit)
				allowed = rule->allow;
		}
		if (!allowed)
			return false;
	}

	return true;
}

static int flux_oci_parse_memory_resource(
	const char *json, const jsmntok_t *tokens, int index,
	struct flux_oci_memory_resources *memory)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "limit") ||
		    flux_oci_token_eq(json, &tokens[key], "reservation") ||
		    flux_oci_token_eq(json, &tokens[key], "swap")) {
			long long number;
			int64_t *dst;
			bool *has;

			if (flux_oci_token_i64(json, &tokens[value], &number) < 0 ||
			    number < -1)
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "limit")) {
				dst = &memory->limit;
				has = &memory->has_limit;
			} else if (flux_oci_token_eq(json, &tokens[key],
						       "reservation")) {
				dst = &memory->reservation;
				has = &memory->has_reservation;
			} else {
				dst = &memory->swap;
				has = &memory->has_swap;
			}
			*dst = (int64_t)number;
			*has = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "swappiness")) {
			unsigned long long number;

			if (flux_oci_token_u64(json, &tokens[value], &number) < 0 ||
			    number > 100)
				return -1;
			memory->swappiness = (uint64_t)number;
			memory->has_swappiness = true;
		} else if (flux_oci_token_eq(json, &tokens[key],
						      "disableOOMKiller") ||
			   flux_oci_token_eq(json, &tokens[key], "useHierarchy") ||
			   flux_oci_token_eq(json, &tokens[key],
						      "checkBeforeUpdate")) {
			bool setting;
			bool *dst;
			bool *has;

			if (flux_oci_parse_bool(json, &tokens[value], &setting) < 0)
				return -1;
			if (flux_oci_token_eq(json, &tokens[key],
						 "disableOOMKiller")) {
				dst = &memory->disable_oom_killer;
				has = &memory->has_disable_oom_killer;
			} else if (flux_oci_token_eq(json, &tokens[key],
							"useHierarchy")) {
				dst = &memory->use_hierarchy;
				has = &memory->has_use_hierarchy;
			} else {
				dst = &memory->check_before_update;
				has = &memory->has_check_before_update;
			}
			*dst = setting;
			*has = true;
		} else {
			char *field = NULL;
			char scoped[192];
			int ret;

			if (flux_oci_token_str(json, &tokens[key], &field) < 0)
				return -1;
			if (snprintf(scoped, sizeof(scoped),
				     "linux.resources.memory.%s", field) >=
			    (int)sizeof(scoped)) {
				free(field);
				return -1;
			}
			ret = flux_oci_reject_nonempty(json, &tokens[value], scoped);
			free(field);
			if (ret < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

static int flux_oci_parse_cpu_resource(const char *json,
				       const jsmntok_t *tokens, int index,
				       struct flux_oci_cpu_resources *cpu)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "shares") ||
		    flux_oci_token_eq(json, &tokens[key], "period") ||
		    flux_oci_token_eq(json, &tokens[key], "burst") ||
		    flux_oci_token_eq(json, &tokens[key], "realtimePeriod")) {
			unsigned long long number;
			uint64_t *dst;
			bool *has;

			if (flux_oci_token_u64(json, &tokens[value], &number) < 0)
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "shares")) {
				if (number != 0 && (number < 2 || number > 262144))
					return -1;
				dst = &cpu->shares;
				has = &cpu->has_shares;
			} else if (flux_oci_token_eq(json, &tokens[key], "period")) {
				dst = &cpu->period;
				has = &cpu->has_period;
			} else if (flux_oci_token_eq(json, &tokens[key], "burst")) {
				dst = &cpu->burst;
				has = &cpu->has_burst;
			} else {
				dst = &cpu->realtime_period;
				has = &cpu->has_realtime_period;
			}
			*dst = (uint64_t)number;
			*has = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "quota") ||
			   flux_oci_token_eq(json, &tokens[key],
						      "realtimeRuntime") ||
			   flux_oci_token_eq(json, &tokens[key], "idle")) {
			long long number;
			int64_t *dst;
			bool *has;

			if (flux_oci_token_i64(json, &tokens[value], &number) < 0 ||
			    number < -1)
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "quota")) {
				dst = &cpu->quota;
				has = &cpu->has_quota;
			} else if (flux_oci_token_eq(json, &tokens[key],
							"realtimeRuntime")) {
				dst = &cpu->realtime_runtime;
				has = &cpu->has_realtime_runtime;
			} else {
				if (number != 0 && number != 1)
					return -1;
				dst = &cpu->idle;
				has = &cpu->has_idle;
			}
			*dst = (int64_t)number;
			*has = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "cpus") ||
			   flux_oci_token_eq(json, &tokens[key], "mems")) {
			bool is_cpus = flux_oci_token_eq(json, &tokens[key], "cpus");
			char **dst = is_cpus ? &cpu->cpus : &cpu->mems;

			if (tokens[value].type != JSMN_STRING ||
			    flux_oci_token_str(json, &tokens[value], dst) < 0)
				return -1;
			if (is_cpus)
				cpu->has_cpus = true;
			else
				cpu->has_mems = true;
		} else {
			char *field = NULL;
			char scoped[160];
			int ret;

			if (flux_oci_token_str(json, &tokens[key], &field) < 0)
				return -1;
			ret = snprintf(scoped, sizeof(scoped),
				       "linux.resources.cpu.%s", field);
			free(field);
			if (ret < 0 || ret >= (int)sizeof(scoped) ||
			    flux_oci_reject_nonempty(json, &tokens[value], scoped) < 0)
				return -1;
		}
		tok = flux_oci_token_skip(tokens, value);
	}

	if (cpu->has_quota && cpu->quota > 0 && cpu->has_burst &&
	    cpu->burst > (uint64_t)cpu->quota)
		return -1;
	return 0;
}

static int flux_oci_parse_pids_resource(const char *json,
					const jsmntok_t *tokens, int index,
					struct flux_oci_pids_resources *pids)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;
	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "limit")) {
			long long number;

			if (flux_oci_token_i64(json, &tokens[value], &number) < 0 ||
			    number < -1)
				return -1;
			pids->limit = (int64_t)number;
			pids->has_limit = true;
		} else {
			return -1;
		}
		tok = flux_oci_token_skip(tokens, value);
	}
	return pids->has_limit ? 0 : -1;
}

static int flux_oci_parse_block_io_throttle(
	const char *json, const jsmntok_t *tokens, int index,
	struct flux_oci_block_io_throttle *throttle)
{
	bool has_major = false;
	bool has_minor = false;
	bool has_rate = false;
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;
	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "major") ||
		    flux_oci_token_eq(json, &tokens[key], "minor")) {
			long long number;
			if (flux_oci_token_i64(json, &tokens[value], &number) < 0 ||
			    number < 0)
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "major")) {
				throttle->major = (int64_t)number;
				has_major = true;
			} else {
				throttle->minor = (int64_t)number;
				has_minor = true;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "rate")) {
			unsigned long long number;
			if (flux_oci_token_u64(json, &tokens[value], &number) < 0 ||
			    number == 0)
				return -1;
			throttle->rate = (uint64_t)number;
			has_rate = true;
		} else {
			return -1;
		}
		tok = flux_oci_token_skip(tokens, value);
	}
	return has_major && has_minor && has_rate ? 0 : -1;
}

static int flux_oci_parse_block_io_throttles(
	const char *json, const jsmntok_t *tokens, int index,
	struct flux_oci_block_io_throttle **items, int *count)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_ARRAY)
		return -1;
	free(*items);
	*items = NULL;
	*count = 0;
	if (tokens[index].size > 0) {
		*items = calloc((size_t)tokens[index].size, sizeof(**items));
		if (!*items)
			return -1;
	}
	for (i = 0; i < tokens[index].size; i++) {
		if (flux_oci_parse_block_io_throttle(json, tokens, tok,
						     &(*items)[i]) < 0)
			return -1;
		(*count)++;
		tok = flux_oci_token_skip(tokens, tok);
	}
	return 0;
}

static int flux_oci_parse_block_io_weight_device(
	const char *json, const jsmntok_t *tokens, int index,
	struct flux_oci_block_io_weight_device *device)
{
	bool has_major = false;
	bool has_minor = false;
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;
	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "major") ||
		    flux_oci_token_eq(json, &tokens[key], "minor")) {
			long long number;
			if (flux_oci_token_i64(json, &tokens[value], &number) < 0 ||
			    number < 0)
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "major")) {
				device->major = (int64_t)number;
				has_major = true;
			} else {
				device->minor = (int64_t)number;
				has_minor = true;
			}
		} else if (flux_oci_token_eq(json, &tokens[key], "weight") ||
			   flux_oci_token_eq(json, &tokens[key], "leafWeight")) {
			unsigned long long number;
			uint16_t *dst;
			bool *has;

			if (flux_oci_token_u64(json, &tokens[value], &number) < 0 ||
			    (number != 0 && (number < 10 || number > 1000)))
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "weight")) {
				dst = &device->weight;
				has = &device->has_weight;
			} else {
				dst = &device->leaf_weight;
				has = &device->has_leaf_weight;
			}
			*dst = (uint16_t)number;
			*has = true;
		} else {
			return -1;
		}
		tok = flux_oci_token_skip(tokens, value);
	}
	return has_major && has_minor &&
	       (device->has_weight || device->has_leaf_weight) ? 0 : -1;
}

static int flux_oci_parse_block_io_resource(
	const char *json, const jsmntok_t *tokens, int index,
	struct flux_oci_block_io_resources *block_io)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;
	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "weight") ||
		    flux_oci_token_eq(json, &tokens[key], "leafWeight")) {
			unsigned long long number;
			uint16_t *dst;
			bool *has;

			if (flux_oci_token_u64(json, &tokens[value], &number) < 0 ||
			    (number != 0 && (number < 10 || number > 1000)))
				return -1;
			if (flux_oci_token_eq(json, &tokens[key], "weight")) {
				dst = &block_io->weight;
				has = &block_io->has_weight;
			} else {
				dst = &block_io->leaf_weight;
				has = &block_io->has_leaf_weight;
			}
			*dst = (uint16_t)number;
			*has = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "weightDevice")) {
			int j;
			int item = value + 1;
			if (tokens[value].type != JSMN_ARRAY)
				return -1;
			free(block_io->weight_devices);
			block_io->weight_devices = calloc(
				(size_t)tokens[value].size,
				sizeof(*block_io->weight_devices));
			if (tokens[value].size > 0 && !block_io->weight_devices)
				return -1;
			block_io->weight_device_num = 0;
			for (j = 0; j < tokens[value].size; j++) {
				if (flux_oci_parse_block_io_weight_device(
					    json, tokens, item,
					    &block_io->weight_devices[j]) < 0)
					return -1;
				block_io->weight_device_num++;
				item = flux_oci_token_skip(tokens, item);
			}
		} else if (flux_oci_token_eq(json, &tokens[key],
						      "throttleReadBpsDevice")) {
			if (flux_oci_parse_block_io_throttles(
				    json, tokens, value, &block_io->throttle_read_bps,
				    &block_io->throttle_read_bps_num) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key],
						      "throttleWriteBpsDevice")) {
			if (flux_oci_parse_block_io_throttles(
				    json, tokens, value, &block_io->throttle_write_bps,
				    &block_io->throttle_write_bps_num) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key],
						      "throttleReadIOPSDevice")) {
			if (flux_oci_parse_block_io_throttles(
				    json, tokens, value, &block_io->throttle_read_iops,
				    &block_io->throttle_read_iops_num) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key],
						      "throttleWriteIOPSDevice")) {
			if (flux_oci_parse_block_io_throttles(
				    json, tokens, value, &block_io->throttle_write_iops,
				    &block_io->throttle_write_iops_num) < 0)
				return -1;
		} else {
			return -1;
		}
		tok = flux_oci_token_skip(tokens, value);
	}
	return 0;
}

static int flux_oci_parse_network_resource(
	const char *json, const jsmntok_t *tokens, int index,
	struct flux_oci_network_resources *network)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;
	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "classID")) {
			unsigned long long number;
			if (flux_oci_token_u64(json, &tokens[value], &number) < 0 ||
			    number > UINT32_MAX)
				return -1;
			network->class_id = (uint32_t)number;
			network->has_class_id = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "priorities")) {
			int j;
			int item = value + 1;
			if (tokens[value].type != JSMN_ARRAY)
				return -1;
			network->priorities = calloc(
				(size_t)tokens[value].size,
				sizeof(*network->priorities));
			if (tokens[value].size > 0 && !network->priorities)
				return -1;
			for (j = 0; j < tokens[value].size; j++) {
				struct flux_oci_network_priority *priority =
					&network->priorities[j];
				bool has_name = false;
				bool has_priority = false;
				int k;
				int field = item + 1;

				if (tokens[item].type != JSMN_OBJECT)
					return -1;
				for (k = 0; k < tokens[item].size; k++) {
					int field_value = field + 1;
					if (flux_oci_token_eq(json, &tokens[field],
							      "name")) {
						if (tokens[field_value].type != JSMN_STRING ||
						    flux_oci_token_str(json,
							&tokens[field_value],
							&priority->name) < 0)
							return -1;
						has_name = priority->name[0] != '\0';
					} else if (flux_oci_token_eq(
							   json, &tokens[field],
							   "priority")) {
						unsigned long long number;
						if (flux_oci_token_u64(
							    json, &tokens[field_value],
							    &number) < 0 ||
						    number > UINT32_MAX)
							return -1;
						priority->priority = (uint32_t)number;
						has_priority = true;
					} else {
						return -1;
					}
					field = flux_oci_token_skip(tokens, field_value);
				}
				if (!has_name || !has_priority)
					return -1;
				network->priority_num++;
				item = flux_oci_token_skip(tokens, item);
			}
		} else {
			return -1;
		}
		tok = flux_oci_token_skip(tokens, value);
	}
	return 0;
}

static int flux_oci_parse_resources(const char *json,
				    const jsmntok_t *tokens, int index,
				    struct flux_oci_resources *resources)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;

		if (flux_oci_token_eq(json, &tokens[key], "devices")) {
			if (flux_oci_parse_device_rules(json, tokens, value) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "memory")) {
			if (flux_oci_parse_memory_resource(
				    json, tokens, value, &resources->memory) < 0)
				return -1;
			resources->has_memory = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "cpu")) {
			if (flux_oci_parse_cpu_resource(json, tokens, value,
						&resources->cpu) < 0)
				return -1;
			resources->has_cpu = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "pids")) {
			if (flux_oci_parse_pids_resource(json, tokens, value,
						 &resources->pids) < 0)
				return -1;
			resources->has_pids = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "blockIO")) {
			if (flux_oci_parse_block_io_resource(
				    json, tokens, value, &resources->block_io) < 0)
				return -1;
			resources->has_block_io = true;
		} else if (flux_oci_token_eq(json, &tokens[key], "network")) {
			if (flux_oci_parse_network_resource(
				    json, tokens, value, &resources->network) < 0)
				return -1;
			resources->has_network = true;
		} else {
			char *field = NULL;
			char scoped[160];
			int ret;

			if (flux_oci_token_str(json, &tokens[key], &field) < 0)
				return -1;
			if (snprintf(scoped, sizeof(scoped),
				     "linux.resources.%s", field) >=
			    (int)sizeof(scoped)) {
				free(field);
				return -1;
			}
			ret = flux_oci_reject_nonempty(json, &tokens[value], scoped);
			free(field);
			if (ret < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	return 0;
}

int flux_oci_resources_parse_json(const char *json,
				  struct flux_oci_resources *resources)
{
	struct flux_oci_resources parsed = { 0 };
	jsmn_parser parser;
	jsmntok_t *tokens;
	int token_count;
	int ret = -1;

	if (!json || !resources)
		return -EINVAL;
	tokens = calloc(FLUX_OCI_TOKEN_MAX, sizeof(*tokens));
	if (!tokens)
		return -ENOMEM;
	jsmn_init(&parser);
	token_count = jsmn_parse(&parser, json, strlen(json), tokens,
				 FLUX_OCI_TOKEN_MAX);
	if (token_count < 1 || tokens[0].type != JSMN_OBJECT)
		goto out;
	if (flux_oci_parse_resources(json, tokens, 0, &parsed) < 0)
		goto out;

	flux_oci_resources_fini(resources);
	*resources = parsed;
	memset(&parsed, 0, sizeof(parsed));
	ret = 0;
out:
	flux_oci_resources_fini(&parsed);
	free(tokens);
	return ret;
}

static int flux_oci_parse_cgroups_path(const char *json,
				       const jsmntok_t *tok)
{
	char *path = NULL;
	const char *component;

	if (!tok || tok->type != JSMN_STRING ||
	    flux_oci_token_str(json, tok, &path) < 0)
		return -1;
	if (!path[0]) {
		free(path);
		return 0;
	}
	if (path[0] != '/' || !path[1] || path[strlen(path) - 1] == '/')
		goto invalid;

	component = path + 1;
	while (*component) {
		const char *slash = strchr(component, '/');
		size_t len = slash ? (size_t)(slash - component) :
				     strlen(component);

		if (len == 0 || (len == 1 && component[0] == '.') ||
		    (len == 2 && component[0] == '.' && component[1] == '.'))
			goto invalid;
		component = slash ? slash + 1 : component + len;
	}

	free(flux_oci_cfg.cgroups_path);
	flux_oci_cfg.cgroups_path = path;
	return 0;

invalid:
	FLUX_LOG(FLUX_LOG_ERR,
		 "unsupported OCI cgroupsPath (absolute cgroupfs path required): %s\n",
		 path);
	free(path);
	return -1;
}

static bool flux_oci_sysctl_name_valid(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!name || !name[0] || name[0] == '.' ||
	    name[strlen(name) - 1] == '.')
		return false;
	for (; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
		    *p == '.')
			continue;
		return false;
	}
	return strstr(name, "..") == NULL;
}

static int flux_oci_parse_sysctls(const char *json,
				  const jsmntok_t *tokens, int index)
{
	int i;
	int tok = index + 1;

	if (tokens[index].type != JSMN_OBJECT)
		return -1;

	flux_oci_free_sysctls();
	if (tokens[index].size > 0) {
		flux_oci_cfg.sysctls = calloc((size_t)tokens[index].size,
					       sizeof(*flux_oci_cfg.sysctls));
		if (!flux_oci_cfg.sysctls)
			return -1;
	}

	for (i = 0; i < tokens[index].size; i++) {
		int key = tok;
		int value = key + 1;
		int j;

		flux_oci_cfg.sysctl_num = i + 1;
		if (tokens[key].type != JSMN_STRING ||
		    tokens[value].type != JSMN_STRING ||
		    flux_oci_token_str(json, &tokens[key],
				       &flux_oci_cfg.sysctls[i].name) < 0 ||
		    flux_oci_token_str(json, &tokens[value],
				       &flux_oci_cfg.sysctls[i].value) < 0)
			return -1;
		if (!flux_oci_sysctl_name_valid(flux_oci_cfg.sysctls[i].name))
			return -1;
		for (j = 0; j < i; j++) {
			if (!strcmp(flux_oci_cfg.sysctls[j].name,
				    flux_oci_cfg.sysctls[i].name))
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
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
		} else if (flux_oci_token_eq(json, &tokens[key], "namespaces")) {
			if (flux_oci_parse_namespaces(json, tokens, value) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "resources")) {
			if (flux_oci_parse_resources(json, tokens, value,
						&flux_oci_cfg.resources) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "cgroupsPath")) {
			if (flux_oci_parse_cgroups_path(json, &tokens[value]) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "sysctl")) {
			if (flux_oci_parse_sysctls(json, tokens, value) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "seccomp") ||
			   flux_oci_token_eq(json, &tokens[key], "uidMappings") ||
			   flux_oci_token_eq(json, &tokens[key], "gidMappings") ||
			   flux_oci_token_eq(json, &tokens[key], "devices") ||
			   flux_oci_token_eq(json, &tokens[key], "timeOffsets") ||
			   flux_oci_token_eq(json, &tokens[key], "intelRdt") ||
			   flux_oci_token_eq(json, &tokens[key], "personality")) {
			char *field = NULL;
			char scoped[128];
			int ret;

			if (flux_oci_token_str(json, &tokens[key], &field) < 0)
				return -1;
			if (snprintf(scoped, sizeof(scoped), "linux.%s", field) >=
			    (int)sizeof(scoped)) {
				free(field);
				return -1;
			}
			ret = flux_oci_reject_nonempty(json, &tokens[value], scoped);
			free(field);
			if (ret < 0)
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
	if (flux_oci_cfg.has_console_size && !flux_oci_cfg.terminal) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "process.consoleSize requires process.terminal=true\n");
		return -1;
	}

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

		if (flux_oci_token_eq(json, &tokens[key], "ociVersion")) {
			if (flux_oci_parse_version(json, &tokens[value]) < 0)
				return -1;
		} else if (flux_oci_token_eq(json, &tokens[key], "root")) {
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
		} else if (flux_oci_token_eq(json, &tokens[key], "hooks")) {
			if (flux_oci_reject_nonempty(json, &tokens[value], "hooks") < 0)
				return -1;
		}

		tok = flux_oci_token_skip(tokens, value);
	}

	if (!flux_oci_cfg.oci_version) {
		FLUX_LOG(FLUX_LOG_ERR, "missing required OCI field: ociVersion\n");
		return -1;
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
