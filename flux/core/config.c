#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define _HAVE_STRING_ARCH_strtok_r
#include <string.h>

#include <flux.h>

#include <utils/jsmn.h>

static int flux_run_cfg_cleanup(struct flux_run_cfg *cfg);

static int cfgncpy(char **to, const char *from, int len)
{
	if (!from)
		return 0;
	if (*to)
		free(*to);
	*to = (char *)malloc((len + 1) * sizeof(char));
	if (*to == NULL)
		return -1;
	memcpy(*to, from, len);
	(*to)[len] = '\0';
	return 0;
}

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static struct flux_run_cfg *flux_run_cfg_args_cache;

enum flux_cfg_value_type {
	CFG_VALUE_STRING,
	CFG_VALUE_LIST,
};

enum flux_cfg_option {
	OPT_MULTIPROC = 256,
	OPT_DEBUG,
	OPT_MOUNT,
	OPT_SYSCTL,
	OPT_BOOT_CMDLINE,
	OPT_DUMP,
	OPT_MEM_SIZE,
	OPT_DMA_SIZE,
	OPT_SPDK_BDF,
	OPT_SPDK_ZERO_COPY,
	OPT_SPDK_FS_TYPE,
	OPT_SYNC_ON_SHUTDOWN,
	OPT_HOSTFS_MOUNTS,
	OPT_HOSTFS_WRITABLE,
	OPT_SIGNALS,
	OPT_LD_PATH,
	OPT_ENV,
	OPT_NIC_IP_ADDR,
	OPT_NIC_IP_GW,
	OPT_NIC_IP_MASK,
	OPT_IOK_SOCK_PATH,
};

struct flux_cfg_entry {
	const char *key;
	const char *long_opt;
	int opt;
	enum flux_cfg_value_type type;
	size_t offset;
	size_t count_offset;
	const char *usage;
	int repeatable;
};

#define FLUX_CFG_ENTRIES                                                     \
	X_STR(debug, "debug", OPT_DEBUG, "Enable debug output (0/1)")        \
	X_STR(mount, "mount", OPT_MOUNT, "Extra mount directives")           \
	X_STR(sysctl, "sysctl", OPT_SYSCTL, "sysctl write string")          \
	X_STR(boot_cmdline, "boot_cmdline", OPT_BOOT_CMDLINE,                \
	      "Extra command line for Linux boot")                           \
	X_STR(dump, "dump", OPT_DUMP, "Dump run cfg/state")             \
	X_STR(mem_size, "mem_size", OPT_MEM_SIZE,                            \
	      "Size of normal kernel memory passed via boot cmdline")        \
	X_STR(dma_size, "dma_size", OPT_DMA_SIZE,                            \
	      "Size of the kernel DMA zone passed via boot cmdline")         \
	X_LIST(spdk_bdf, spdk_dev_num, "spdk_bdf", OPT_SPDK_BDF,             \
	       "SPDK block device BDF")                                      \
	X_STR(spdk_zero_copy, "spdk_zero_copy", OPT_SPDK_ZERO_COPY,          \
	      "Enable direct memory allocation from SPDK mempool")           \
	X_STR(spdk_fs_type, "spdk_fs_type", OPT_SPDK_FS_TYPE,                \
	      "FS type on SPDK block device")                                \
	X_STR(sync_on_shutdown, "sync_on_shutdown", OPT_SYNC_ON_SHUTDOWN,    \
	      "Sync filesystem on shutdown (0/1)")                           \
	X_LIST(hostfs_mounts, hostfs_mounts_num, "hostfs_mounts",            \
	       OPT_HOSTFS_MOUNTS,                                            \
	       "Additional hostfs mount in <host_path>:<flux_path> format")  \
	X_STR(hostfs_writable, "hostfs_writable", OPT_HOSTFS_WRITABLE,       \
	      "Allow hostfs mounts to be writable (0/1, default 0)")         \
	X_LIST(signals, signals_num, "signals", OPT_SIGNALS,                 \
	       "Signals handled by host bridge (e.g. SIGSEGV, repeatable)")  \
	X_STR(ld_path, "ld_path", OPT_LD_PATH,                               \
	      "Dynamic loader path (used directly as interp path)")          \
	X_LIST(env, env_num, "env", OPT_ENV,                                 \
	       "Environment entry in VAR=VAL format")                        \
	X_STR(nic_ip_addr, "nic_ip_addr", OPT_NIC_IP_ADDR,                   \
	      "Guest NIC IP address")                                        \
	X_STR(nic_ip_gw, "nic_ip_gw", OPT_NIC_IP_GW,                         \
	      "Guest NIC gateway address")                                   \
	X_STR(nic_ip_mask, "nic_ip_mask", OPT_NIC_IP_MASK,                   \
	      "Guest NIC subnet mask")                                       \
	X_STR(iok_sock_path, "iok_sock_path", OPT_IOK_SOCK_PATH,             \
	      "External iokd control socket path")

static const struct flux_cfg_entry flux_cfg_entries[] = {
#define X_STR(field, optname, optid, desc)               \
	{ .key = #field,                                 \
	  .long_opt = optname,                           \
	  .opt = optid,                                  \
	  .type = CFG_VALUE_STRING,                      \
	  .offset = offsetof(struct flux_run_cfg, field),  \
	  .count_offset = 0,                             \
	  .usage = desc,                                 \
	  .repeatable = 0 },
#define X_LIST(field, count_field, optname, optid, desc)             \
	{ .key = #field,                                             \
	  .long_opt = optname,                                       \
	  .opt = optid,                                              \
	  .type = CFG_VALUE_LIST,                                    \
	  .offset = offsetof(struct flux_run_cfg, field),              \
	  .count_offset = offsetof(struct flux_run_cfg, count_field),  \
	  .usage = desc,                                             \
	  .repeatable = 1 },
	FLUX_CFG_ENTRIES
#undef X_STR
#undef X_LIST
};

static void flux_run_cfg_print_usage(void)
{
	size_t max_opt_len = 0;

	printf("Usage: flux [options] [ENV=VAL ...] [args...]\n");
	printf("Options:\n");
	printf("  -c, --run-cfg <file>       Run cfg JSON file\n");
	printf("  -h, --help                 Print this help message\n");
	printf("      --multiproc            Enable multiprocess bootstrap mode\n");
	for (size_t i = 0; i < ARRAY_SIZE(flux_cfg_entries); i++) {
		const struct flux_cfg_entry *entry = &flux_cfg_entries[i];
		size_t opt_len = strlen(entry->long_opt) + strlen("--") +
				 strlen(" <val>");

		if (entry->repeatable)
			opt_len += strlen(" (repeatable)");

		if (opt_len > max_opt_len)
			max_opt_len = opt_len;
	}
	for (size_t i = 0; i < ARRAY_SIZE(flux_cfg_entries); i++) {
		const struct flux_cfg_entry *entry = &flux_cfg_entries[i];
		size_t opt_len = strlen(entry->long_opt) + strlen("--") +
				 strlen(" <val>");
		size_t pad;

		if (entry->repeatable)
			opt_len += strlen(" (repeatable)");

		pad = (max_opt_len > opt_len) ? (max_opt_len - opt_len) : 0;

		printf("      --%s <val>%s%*s %s\n", entry->long_opt,
		       entry->repeatable ? " (repeatable)" : "", (int)(pad + 1),
		       "", entry->usage);
	}
}

static int cfg_set_string(char **dst, const char *src)
{
	return cfgncpy(dst, src, strlen(src));
}

static int cfg_append_string_list(char ***list, int *count, const char *value)
{
	char **new_list;
	int new_count = *count + 1;

	new_list = realloc(*list, sizeof(char *) * new_count);
	if (!new_list)
		return -1;

	new_list[new_count - 1] = NULL;
	if (cfgncpy(&new_list[new_count - 1], value, strlen(value)) < 0)
		return -1;

	*list = new_list;
	*count = new_count;
	return 0;
}

static struct flux_run_cfg *flux_run_cfg_get_args(void)
{
	if (!flux_run_cfg_args_cache) {
		flux_run_cfg_args_cache =
			calloc(1, sizeof(*flux_run_cfg_args_cache));
	}
	return flux_run_cfg_args_cache;
}

struct flux_run_cfg *flux_run_cfg_from_args(void)
{
	return flux_run_cfg_args_cache;
}

static int cfg_append_string_lists(char ***dst_list, int *dst_count,
				   char *const *src_list, int src_count)
{
	char **new_list = NULL;
	int old_count;
	int new_count;
	int i;

	if (!dst_list || !dst_count || src_count <= 0 || !src_list)
		return 0;

	old_count = *dst_count;
	new_count = old_count + src_count;
	new_list = calloc(new_count, sizeof(char *));
	if (!new_list)
		return -1;

	for (i = 0; i < old_count; i++) {
		if (cfgncpy(&new_list[i], (*dst_list)[i],
			    strlen((*dst_list)[i])) < 0)
			goto out_free;
	}

	for (i = 0; i < src_count; i++) {
		if (cfgncpy(&new_list[old_count + i], src_list[i],
			    strlen(src_list[i])) < 0)
			goto out_free;
	}

	if (*dst_list) {
		for (i = 0; i < old_count; i++)
			free((*dst_list)[i]);
		free(*dst_list);
	}

	*dst_list = new_list;
	*dst_count = new_count;

	return 0;

out_free:
	for (i = 0; i < new_count; i++)
		free(new_list[i]);
	free(new_list);
	return -1;
}

int flux_run_cfg_apply_overrides(struct flux_run_cfg *base,
				 const struct flux_run_cfg *overrides)
{
	const struct flux_cfg_entry *entry;
	size_t i;

	if (!base || !overrides)
		return 0;

	for (i = 0; i < ARRAY_SIZE(flux_cfg_entries); i++) {
		entry = &flux_cfg_entries[i];

		if (entry->type == CFG_VALUE_LIST) {
			char ***dst_list =
				(char ***)((char *)base + entry->offset);
			int *dst_count =
				(int *)((char *)base + entry->count_offset);
			char **src_list =
				*(char ***)((char *)overrides + entry->offset);
			int src_count = *(int *)((char *)overrides +
						 entry->count_offset);

			if (src_count <= 0)
				continue;

			if (cfg_append_string_lists(dst_list, dst_count,
						    src_list, src_count) < 0)
				return -1;
			continue;
		}

		char *src = *(char **)((char *)overrides + entry->offset);
		char **dst = (char **)((char *)base + entry->offset);

		if (!src)
			continue;

		if (cfg_set_string(dst, src) < 0)
			return -1;
	}

	return 0;
}

static const struct flux_cfg_entry *flux_cfg_find_by_opt(int opt)
{
	for (size_t i = 0; i < ARRAY_SIZE(flux_cfg_entries); i++) {
		if (flux_cfg_entries[i].opt == opt)
			return &flux_cfg_entries[i];
	}

	return NULL;
}

static const struct flux_cfg_entry *flux_cfg_find_by_key(const char *json,
							 const jsmntok_t *tok)
{
	size_t key_len;

	if (tok->type != JSMN_STRING)
		return NULL;

	key_len = (size_t)(tok->end - tok->start);

	for (size_t i = 0; i < ARRAY_SIZE(flux_cfg_entries); i++) {
		const struct flux_cfg_entry *entry = &flux_cfg_entries[i];

		if (strlen(entry->key) == key_len &&
		    strncmp(json + tok->start, entry->key, key_len) == 0)
			return entry;
	}

	return NULL;
}

int flux_run_cfg_parse_args(int argc, char **argv)
{
	static struct option options[] = {
		{ "run-cfg", required_argument, 0, 'c' },
		{ "help", no_argument, 0, 'h' },
		{ "multiproc", no_argument, 0, OPT_MULTIPROC },
#define X_STR(field, optname, optid, desc) \
	{ optname, required_argument, 0, optid },
#define X_LIST(field, count_field, optname, optid, desc) \
	{ optname, required_argument, 0, optid },
		FLUX_CFG_ENTRIES
#undef X_STR
#undef X_LIST
		{ NULL, 0, 0, 0 },
	};
	int opt;
	struct flux_run_cfg *cfg;
	const struct flux_cfg_entry *entry;

	optind = 1;

	while ((opt = getopt_long(argc, argv, "+c:h", options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			setenv("FLUX_RUN_CFG_FILE", optarg, 1);
			break;
		case 'h':
			flux_run_cfg_print_usage();
			return 1;
		case OPT_MULTIPROC:
			flux_env.multiproc_enabled = true;
			break;
		default:
			entry = flux_cfg_find_by_opt(opt);
			if (!entry) {
				flux_run_cfg_print_usage();
				return -1;
			}

			cfg = flux_run_cfg_get_args();
			if (!cfg)
				return -1;

			if (entry->type == CFG_VALUE_LIST) {
				char ***list =
					(char ***)((char *)cfg + entry->offset);
				int *count = (int *)((char *)cfg +
						     entry->count_offset);

				if (cfg_append_string_list(list, count,
							   optarg) < 0)
					return -1;
			} else {
				char **dst =
					(char **)((char *)cfg + entry->offset);

				if (cfg_set_string(dst, optarg) < 0)
					return -1;
			}
			break;
		}
	}

	return 0;
}

static int parse_size(uint64_t *size, const char *str)
{
	char c;
	uint32_t m;
	char *new_str = NULL;

	new_str = strdup(str);

	*size = 0;
	c = toupper(str[strlen(str) - 1]);

	switch (c) {
	case 'K':
		m = KB;
		break;

	case 'M':
		m = MB;
		break;

	case 'G':
		m = GB;
		break;

	default:
		m = 1;
	}

	if (m != 1)
		new_str[strlen(str) - 1] = '\0';

	*size = strtoul(new_str, NULL, 0);
	*size *= m;

	free(new_str);

	return 0;
}

int flux_run_cfg_load_json(struct flux_run_cfg *cfg, const char *jstr)
{
	int ret = 0, i;
	unsigned int pos;
	jsmn_parser jp;
	jsmntok_t toks[FLUX_RUN_CFG_JSON_TOKEN_MAX];
	const struct flux_cfg_entry *entry;

	if (!cfg || !jstr)
		return -1;

	jsmn_init(&jp);
	ret = jsmn_parse(&jp, jstr, strlen(jstr), toks, ARRAY_SIZE(toks));
	if (ret < 0)
		return -1;

	if (toks[0].type != JSMN_OBJECT)
		return -1;

	for (pos = 1; pos < jp.toknext; pos++) {
		entry = flux_cfg_find_by_key(jstr, &toks[pos]);
		if (!entry) {
			FLUX_LOG(FLUX_LOG_ERR, "unexpected key in json %.*s\n",
				 toks[pos].end - toks[pos].start,
				 jstr + toks[pos].start);
			goto out_cleanup;
		}

		if (pos + 1 >= (unsigned int)jp.toknext) {
			FLUX_LOG(FLUX_LOG_ERR, "missing value for key %s\n",
				 entry->key);
			goto out_cleanup;
		}

		if (entry->type == CFG_VALUE_LIST) {
			char ***list = (char ***)((char *)cfg + entry->offset);
			int *count = (int *)((char *)cfg + entry->count_offset);

			if (toks[pos + 1].type != JSMN_ARRAY) {
				FLUX_LOG(FLUX_LOG_DEBUG, "%s is not an array\n",
					 entry->key);
				goto out_cleanup;
			}

			*count = toks[pos + 1].size;
			*list = calloc(*count, sizeof(char *));
			if (!*list)
				goto out_cleanup;

			for (i = 0; i < *count; i++) {
				ret = cfgncpy(&(*list)[i],
					      jstr + toks[pos + 2 + i].start,
					      toks[pos + 2 + i].end -
						      toks[pos + 2 + i].start);
				if (ret < 0)
					goto out_cleanup;
			}

			pos += toks[pos + 1].size + 1;
			continue;
		}

		pos++;
		ret = cfgncpy((char **)((char *)cfg + entry->offset),
			      jstr + toks[pos].start,
			      toks[pos].end - toks[pos].start);
		if (ret < 0)
			goto out_cleanup;
	}
	return 0;
out_cleanup:
	flux_run_cfg_cleanup(cfg);
	return ret;
}

void flux_run_cfg_show(struct flux_run_cfg *cfg)
{
	int i;

	if (!cfg)
		return;

	printf("run_cfg:\n");

	for (i = 0; i < (int)ARRAY_SIZE(flux_cfg_entries); i++) {
		const struct flux_cfg_entry *entry = &flux_cfg_entries[i];

		if (entry->type == CFG_VALUE_LIST) {
			char **list = *(char ***)((char *)cfg + entry->offset);
			int count = *(int *)((char *)cfg + entry->count_offset);

			if (count > 0) {
				printf("  %s: \n", entry->key);
				for (int j = 0; j < count; j++)
					printf("    [%s]\n", list[j]);
			}
		} else {
			char *value = *(char **)((char *)cfg + entry->offset);

			if (value)
				printf("  %s: %s\n", entry->key, value);
		}
	}
}

static int flux_run_cfg_emit_json_string(FILE *stream, const char *value)
{
	const unsigned char *cursor =
		(const unsigned char *)(value ? value : "");

	if (fputc('"', stream) == EOF)
		return -EIO;

	for (; *cursor; cursor++) {
		switch (*cursor) {
		case '\\':
		case '"':
			if (fprintf(stream, "\\%c", *cursor) < 0)
				return -EIO;
			break;
		case '\b':
			if (fputs("\\b", stream) == EOF)
				return -EIO;
			break;
		case '\f':
			if (fputs("\\f", stream) == EOF)
				return -EIO;
			break;
		case '\n':
			if (fputs("\\n", stream) == EOF)
				return -EIO;
			break;
		case '\r':
			if (fputs("\\r", stream) == EOF)
				return -EIO;
			break;
		case '\t':
			if (fputs("\\t", stream) == EOF)
				return -EIO;
			break;
		default:
			if (*cursor < 0x20) {
				if (fprintf(stream, "\\u%04x",
					    (unsigned int)*cursor) < 0)
					return -EIO;
			} else if (fputc(*cursor, stream) == EOF) {
				return -EIO;
			}
			break;
		}
	}

	if (fputc('"', stream) == EOF)
		return -EIO;

	return 0;
}

static int flux_run_cfg_emit_json(const struct flux_run_cfg *cfg, FILE *stream)
{
	int emitted = 0;
	int i;

	if (fputs("{\n", stream) == EOF)
		return -EIO;

	for (i = 0; i < (int)ARRAY_SIZE(flux_cfg_entries); i++) {
		const struct flux_cfg_entry *entry = &flux_cfg_entries[i];

		if (entry->type == CFG_VALUE_LIST) {
			char *const *list =
				*(char *const *const *)((const char *)cfg +
							entry->offset);
			int count =
				*(const int *)((const char *)cfg +
					      entry->count_offset);
			int j;

			if (count <= 0)
				continue;

			if (fprintf(stream, "%s  \"%s\": [",
				    emitted ? ",\n" : "", entry->key) < 0)
				return -EIO;

			for (j = 0; j < count; j++) {
				if (j && fputs(", ", stream) == EOF)
					return -EIO;
				if (flux_run_cfg_emit_json_string(stream,
								  list[j]) < 0)
					return -EIO;
			}

			if (fputc(']', stream) == EOF)
				return -EIO;
			emitted = 1;
			continue;
		}

		{
			const char *value =
				*(char *const *)((const char *)cfg + entry->offset);

			if (!value)
				continue;

			if (fprintf(stream, "%s  \"%s\": ",
				    emitted ? ",\n" : "", entry->key) < 0)
				return -EIO;
			if (flux_run_cfg_emit_json_string(stream, value) < 0)
				return -EIO;
			emitted = 1;
		}
	}

	if (fputs(emitted ? "\n}\n" : "}\n", stream) == EOF)
		return -EIO;

	return 0;
}

int flux_run_cfg_save_json(const struct flux_run_cfg *cfg, const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *mem = NULL;
	FILE *out = NULL;
	int ret = 0;

	if (!cfg || !path || !path[0])
		return -EINVAL;

	mem = open_memstream(&buf, &len);
	if (!mem)
		return -errno;

	ret = flux_run_cfg_emit_json(cfg, mem);
	if (ret < 0)
		goto out;

	if (fclose(mem) < 0) {
		ret = -errno;
		mem = NULL;
		goto out;
	}
	mem = NULL;

	out = fopen(path, "w");
	if (!out) {
		ret = -errno;
		goto out;
	}

	if (len > 0 && fwrite(buf, 1, len, out) != len) {
		ret = -EIO;
		goto out;
	}

	if (fclose(out) < 0) {
		ret = -errno;
		out = NULL;
		goto out;
	}
	out = NULL;

out:
	if (out)
		fclose(out);
	if (mem)
		fclose(mem);
	free(buf);
	return ret;
}

static void free_cfgparam(char *cfgparam)
{
	if (cfgparam)
		free(cfgparam);
}

static int flux_run_cfg_cleanup(struct flux_run_cfg *cfg)
{
	int i;

	if (!cfg)
		return -1;

	for (i = 0; i < (int)ARRAY_SIZE(flux_cfg_entries); i++) {
		const struct flux_cfg_entry *entry = &flux_cfg_entries[i];

		if (entry->type == CFG_VALUE_LIST) {
			char ***list = (char ***)((char *)cfg + entry->offset);
			int *count = (int *)((char *)cfg + entry->count_offset);

			if (*count > 0) {
				for (int j = 0; j < *count; j++)
					free_cfgparam((*list)[j]);
				free(*list);
				*list = NULL;
				*count = 0;
			}
		} else {
			char **value = (char **)((char *)cfg + entry->offset);

			free_cfgparam(*value);
			*value = NULL;
		}
	}

	return 0;
}

void flux_run_cfg_fini(struct flux_run_cfg *cfg)
{
	if (!cfg)
		return;

	(void)flux_run_cfg_cleanup(cfg);
	memset(cfg, 0, sizeof(*cfg));
}

int flux_run_cfg_apply_pre(struct flux_run_cfg *cfg)
{
	if (!cfg)
		return 0;

	if (cfg->debug)
		flux_env.debug = strtol(cfg->debug, NULL, 0);

	if (cfg->mem_size)
		parse_size(&flux_env.mem_size, cfg->mem_size);

	if (cfg->dma_size)
		parse_size(&flux_env.dma_size, cfg->dma_size);

	if (cfg->spdk_zero_copy)
		flux_env.spdk_zero_copy = atoi(cfg->spdk_zero_copy);

	if (cfg->nic_ip_addr)
		flux_env.fnet_enabled = 1;

	return 0;
}

int flux_run_cfg_apply_post(struct flux_run_cfg *cfg)
{
	if (!cfg)
		return 0;

	if (cfg->sysctl)
		flux_sysctl_parse_write(cfg->sysctl);

	return 0;
}
