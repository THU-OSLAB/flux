#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <flux.h>

#include "../runc.h"

enum flux_runc_output_format {
	FLUX_RUNC_OUTPUT_TABLE = 0,
	FLUX_RUNC_OUTPUT_JSON,
};

struct flux_runc_list_cmd {
	enum flux_runc_output_format format;
	bool quiet;
};

struct flux_runc_list_ctx {
	enum flux_runc_output_format format;
	bool quiet;
	bool first_json;
	bool any;
};

static int flux_runc_list_json_escape(const char *value)
{
	const unsigned char *p;

	if (fputc('"', stdout) == EOF)
		return -EIO;

	for (p = (const unsigned char *)value; p && *p; p++) {
		switch (*p) {
		case '\\':
		case '"':
			if (fprintf(stdout, "\\%c", *p) < 0)
				return -EIO;
			break;
		case '\n':
			if (fputs("\\n", stdout) == EOF)
				return -EIO;
			break;
		case '\r':
			if (fputs("\\r", stdout) == EOF)
				return -EIO;
			break;
		case '\t':
			if (fputs("\\t", stdout) == EOF)
				return -EIO;
			break;
		default:
			if (*p < 0x20) {
				if (fprintf(stdout, "\\u%04x", *p) < 0)
					return -EIO;
			} else if (fputc(*p, stdout) == EOF) {
				return -EIO;
			}
			break;
		}
	}

	if (fputc('"', stdout) == EOF)
		return -EIO;

	return 0;
}

static int flux_runc_list_parse(int argc, char **argv,
				struct flux_runc_list_cmd *cmd)
{
	static struct option options[] = {
		{ "format", required_argument, 0, 'f' },
		{ "quiet", no_argument, 0, 'q' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	memset(cmd, 0, sizeof(*cmd));
	cmd->format = FLUX_RUNC_OUTPUT_TABLE;
	optind = 2;

	while ((opt = getopt_long(argc, argv, "+f:qh", options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			if (!strcmp(optarg, "json")) {
				cmd->format = FLUX_RUNC_OUTPUT_JSON;
			} else if (!strcmp(optarg, "table")) {
				cmd->format = FLUX_RUNC_OUTPUT_TABLE;
			} else {
				return -EINVAL;
			}
			break;
		case 'q':
			cmd->quiet = true;
			break;
		case 'h':
		default:
			return -EINVAL;
		}
	}

	if (argc - optind != 0)
		return -EINVAL;

	return 0;
}

static int flux_runc_list_set_env_root(const char *root, const char *old_root,
				       bool had_old_root)
{
	if (root && setenv("FLUX_RUNC_ROOT", root, 1) < 0)
		return -errno;

	if (!root) {
		if (had_old_root) {
			if (setenv("FLUX_RUNC_ROOT", old_root, 1) < 0)
				return -errno;
		} else if (unsetenv("FLUX_RUNC_ROOT") < 0) {
			return -errno;
		}
	}

	return 0;
}

static int flux_runc_list_load_from_root(struct flux_runc_state *state,
					 const char *root, const char *id)
{
	const char *old_root = getenv("FLUX_RUNC_ROOT");
	char *saved_root = old_root ? strdup(old_root) : NULL;
	int ret;

	if (old_root && !saved_root)
		return -ENOMEM;

	ret = flux_runc_list_set_env_root(root, old_root, saved_root != NULL);
	if (ret < 0)
		goto out;

	ret = flux_runc_state_load(state, id);

out:
	if (saved_root) {
		int restore_ret = flux_runc_list_set_env_root(NULL, saved_root,
							      true);

		if (ret >= 0 && restore_ret < 0)
			ret = restore_ret;
	}

	free(saved_root);
	return ret;
}

static int flux_runc_list_emit_json_entry(const struct flux_runc_state *state)
{
	if (!state)
		return -EINVAL;

	if (fputs("    {\n      \"id\": ", stdout) == EOF)
		return -EIO;
	if (flux_runc_list_json_escape(state->id ?: "") < 0)
		return -EIO;
	if (fputs(",\n      \"pid\": ", stdout) == EOF)
		return -EIO;
	if (fprintf(stdout, "%ld", (long)state->init_pid) < 0)
		return -EIO;
	if (fputs(",\n      \"status\": ", stdout) == EOF)
		return -EIO;
	if (flux_runc_list_json_escape(flux_runc_status_name(state->status)) < 0)
		return -EIO;
	if (fputs(",\n      \"bundle\": ", stdout) == EOF)
		return -EIO;
	if (flux_runc_list_json_escape(state->bundle_dir ?: "") < 0)
		return -EIO;
	if (fputs("\n    }", stdout) == EOF)
		return -EIO;

	return 0;
}

static int flux_runc_list_emit_table_header(void)
{
	if (printf("%-24s %-8s %-10s %s\n", "ID", "PID", "STATUS",
		   "BUNDLE") < 0)
		return -EIO;
	return 0;
}

static int flux_runc_list_emit_table_entry(const struct flux_runc_state *state)
{
	if (!state)
		return -EINVAL;

	if (printf("%-24s %-8ld %-10s %s\n", state->id ?: "", (long)state->init_pid,
		   flux_runc_status_name(state->status),
		   state->bundle_dir ?: "") < 0)
		return -EIO;

	return 0;
}

static int flux_runc_list_emit_quiet_entry(const struct flux_runc_state *state)
{
	if (!state)
		return -EINVAL;

	if (printf("%s\n", state->id ?: "") < 0)
		return -EIO;

	return 0;
}

static int flux_runc_list_consider_root(const char *root,
					const struct flux_runc_list_cmd *cmd,
					struct flux_runc_list_ctx *ctx)
{
	DIR *dir;
	struct dirent *de;
	int ret = 0;

	dir = opendir(root);
	if (!dir)
		return errno == ENOENT ? 0 : -errno;

	while ((de = readdir(dir)) != NULL) {
		struct flux_runc_state state;
		struct stat st;
		char path[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") ||
		    !strcmp(de->d_name, "sessions"))
			continue;

		if (snprintf(path, sizeof(path), "%s/%s", root, de->d_name) >=
		    (int)sizeof(path)) {
			ret = -ENAMETOOLONG;
			break;
		}

		if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;

		flux_runc_state_reset(&state);
		ret = flux_runc_list_load_from_root(&state, root, de->d_name);
		if (ret < 0) {
			flux_runc_state_fini(&state);
			continue;
		}

		ret = flux_runc_reconcile_loaded_state(
			&state, "failed to reconcile runtime state");
		if (ret < 0) {
			flux_runc_state_fini(&state);
			break;
		}

		if (!ctx->any) {
			if (cmd->format == FLUX_RUNC_OUTPUT_JSON) {
				if (fputs("[\n", stdout) == EOF) {
					ret = -EIO;
					flux_runc_state_fini(&state);
					break;
				}
			} else if (!cmd->quiet) {
				ret = flux_runc_list_emit_table_header();
				if (ret < 0) {
					flux_runc_state_fini(&state);
					break;
				}
			}
		}

		if (cmd->quiet) {
			ret = flux_runc_list_emit_quiet_entry(&state);
		} else if (cmd->format == FLUX_RUNC_OUTPUT_JSON) {
			if (!ctx->first_json) {
				if (fputs(",\n", stdout) == EOF)
					ret = -EIO;
			}
			if (ret >= 0)
				ret = flux_runc_list_emit_json_entry(&state);
			ctx->first_json = false;
		} else {
			ret = flux_runc_list_emit_table_entry(&state);
		}
		if (ret < 0) {
			flux_runc_state_fini(&state);
			break;
		}

		ctx->any = true;
		flux_runc_state_fini(&state);
	}

	closedir(dir);
	return ret;
}

static int flux_runc_list_scan_roots(const struct flux_runc_list_cmd *cmd)
{
	const char *env_root = getenv("FLUX_RUNC_ROOT");
	char user_root[PATH_MAX];
	char *tmp_root = NULL;
	uid_t uid = getuid();
	struct flux_runc_list_ctx ctx = {
		.format = cmd->format,
		.quiet = cmd->quiet,
		.first_json = true,
		.any = false,
	};
	int ret;

	if (env_root && env_root[0]) {
		ret = flux_runc_list_consider_root(env_root, cmd, &ctx);
		if (ret < 0)
			return ret;
	} else {
		ret = flux_runc_list_consider_root("/run/flux-runc", cmd, &ctx);
		if (ret < 0)
			return ret;

		if (snprintf(user_root, sizeof(user_root),
			     "/run/user/%lu/flux-runc",
			     (unsigned long)uid) < (int)sizeof(user_root)) {
			ret = flux_runc_list_consider_root(user_root, cmd, &ctx);
			if (ret < 0)
				return ret;
		}

		tmp_root = flux_user_scoped_path_strdup("/tmp/flux-runc");
		if (!tmp_root)
			return -ENOMEM;
		ret = flux_runc_list_consider_root(tmp_root, cmd, &ctx);
		free(tmp_root);
		if (ret < 0)
			return ret;
	}

	if (cmd->format == FLUX_RUNC_OUTPUT_JSON) {
		if (fputs(ctx.any ? "\n]\n" : "]\n", stdout) == EOF)
			return -EIO;
	}

	return 0;
}

int flux_runc_cmd_list(int argc, char **argv)
{
	struct flux_runc_list_cmd cmd;
	int ret;

	ret = flux_runc_list_parse(argc, argv, &cmd);
	if (ret < 0)
		return EXIT_FAILURE;

	ret = flux_runc_list_scan_roots(&cmd);
	return ret < 0 ? EXIT_FAILURE : 0;
}
