#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <flux.h>

#include "runc/runc.h"

struct flux_runc_global_opts {
	const char *root;
	const char *log;
	const char *log_format;
	bool version;
};

static struct flux_runc_global_opts flux_runc_opts;

static bool flux_runc_is_known_command(const char *arg)
{
	static const char *commands[] = {
		"create", "start", "run", "state", "kill", "exec",
		"features",
		"ps", "list", "delete", "__exec",
	};
	size_t i;

	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (!strcmp(arg, commands[i]))
			return true;
	}

	return false;
}

static bool flux_runc_match_long_opt(const char *arg, const char *name,
				     const char **value)
{
	size_t len = strlen(name);

	if (strncmp(arg, name, len))
		return false;

	if (!arg[len]) {
		*value = NULL;
		return true;
	}

	if (arg[len] != '=')
		return false;

	*value = arg + len + 1;
	return true;
}

static int flux_runc_take_opt_value(const char *opt_name, int argc, char **argv,
				    int *index, const char **value)
{
	if (*value)
		return 0;

	if (*index + 1 >= argc) {
		fprintf(stderr, "missing value for %s\n", opt_name);
		return -EINVAL;
	}

	*index += 1;
	*value = argv[*index];
	return 0;
}

static int flux_runc_write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t nw = write(fd, buf, len);

		if (nw < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		buf += nw;
		len -= (size_t)nw;
	}

	return 0;
}

static int flux_runc_write_runtime_error(const char *path, int exit_code)
{
	char buf[256];
	int fd;
	int len;
	int ret;

	if (!path || !path[0])
		return 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;

	len = snprintf(buf, sizeof(buf),
		       "{\"error\":\"flux-runc command failed\",\"exitStatus\":%d}\n",
		       exit_code);
	if (len < 0 || len >= (int)sizeof(buf)) {
		close(fd);
		return -EINVAL;
	}

	ret = flux_runc_write_all(fd, buf, (size_t)len);
	if (close(fd) < 0 && ret == 0)
		ret = -errno;

	return ret;
}

static int flux_runc_normalize_argv(int argc, char **argv, int *out_argc,
				    char ***out_argv)
{
	char **normalized;
	int cmd_index = -1;
	int i;
	int out = 0;

	memset(&flux_runc_opts, 0, sizeof(flux_runc_opts));

	if (argc < 2)
		return 0;

	for (i = 1; i < argc; i++) {
		const char *value = NULL;
		const char *arg = argv[i];
		int ret;

		if (!strcmp(arg, "--")) {
			if (i + 1 < argc)
				cmd_index = i + 1;
			break;
		}

		if (flux_runc_is_known_command(arg)) {
			cmd_index = i;
			break;
		}

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help"))
			return 0;

		if (!strcmp(arg, "--version")) {
			flux_runc_opts.version = true;
			continue;
		}

		if (flux_runc_match_long_opt(arg, "--root", &value)) {
			ret = flux_runc_take_opt_value("--root", argc, argv, &i,
						       &value);
			if (ret)
				return ret;
			flux_runc_opts.root = value;
			continue;
		}

		if (flux_runc_match_long_opt(arg, "--log", &value) ||
		    flux_runc_match_long_opt(arg, "--log-format", &value) ||
		    flux_runc_match_long_opt(arg, "--criu", &value) ||
		    flux_runc_match_long_opt(arg, "--criu-work-path", &value)) {
			ret = flux_runc_take_opt_value(arg, argc, argv, &i,
						       &value);
			if (ret)
				return ret;
			if (!strcmp(arg, "--log"))
				flux_runc_opts.log = value;
			else if (!strcmp(arg, "--log-format"))
				flux_runc_opts.log_format = value;
			continue;
		}

		if (!strcmp(arg, "--debug") || !strcmp(arg, "--systemd-cgroup") ||
		    !strcmp(arg, "--rootless"))
			continue;

		if (!strncmp(arg, "--rootless=", strlen("--rootless=")))
			continue;

		break;
	}

	if (flux_runc_opts.version) {
		fprintf(stdout, "flux-runc\n");
		exit(0);
	}

	if (flux_runc_opts.root &&
	    setenv("FLUX_RUNC_ROOT", flux_runc_opts.root, 1) < 0) {
		perror("setenv(FLUX_RUNC_ROOT)");
		return -errno;
	}

	if (cmd_index <= 1)
		return 0;

	normalized = calloc((size_t)(argc - cmd_index + 2), sizeof(*normalized));
	if (!normalized)
		return -ENOMEM;

	normalized[out++] = argv[0];
	for (i = cmd_index; i < argc; i++)
		normalized[out++] = argv[i];
	normalized[out] = NULL;

	*out_argc = out;
	*out_argv = normalized;
	return 0;
}

static bool flux_runc_is_help_arg(const char *arg)
{
	return !strcmp(arg, "-h") || !strcmp(arg, "--help");
}

static bool flux_runc_wants_help(int argc, char **argv)
{
	if (argc < 2)
		return true;

	if (flux_runc_is_help_arg(argv[1]))
		return true;

	if (argc >= 3 && strcmp(argv[1], "__exec") &&
	    flux_runc_is_help_arg(argv[2]))
		return true;

	return false;
}

static void flux_runc_print_usage(void)
{
	fprintf(stderr,
		"Usage: flux-runc [global options] <command> [options] <container-id>\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  create   Create runtime state from an OCI bundle\n");
	fprintf(stderr, "  start    Start a previously created container\n");
	fprintf(stderr, "  run      Create, start, and optionally wait\n");
	fprintf(stderr, "  state    Show persisted runtime state\n");
	fprintf(stderr, "  kill     Deliver supported OCI signals\n");
	fprintf(stderr, "  exec     Start an additional process inside a running container\n");
	fprintf(stderr, "  features Print supported runtime feature metadata\n");
	fprintf(stderr, "  ps       Show container process ids\n");
	fprintf(stderr, "  list     List known containers\n");
	fprintf(stderr, "  delete   Remove container runtime state\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr,
		"  -b, --bundle <dir>   OCI bundle directory (default: cwd)\n");
	fprintf(stderr,
		"      --console-socket <path>  Send detached terminal pty over a Unix socket\n");
	fprintf(stderr,
		"  -d, --detach         Return after start for `run` only\n");
	fprintf(stderr,
		"                       `run -d` with a terminal requires --console-socket\n");
	fprintf(stderr,
		"  -p, --pid-file <p>   Write or persist the init pid file path\n");
	fprintf(stderr,
		"  -f, --force          Force delete a running container\n");
	fprintf(stderr,
		"  exec: -d/--detach, -p/--process, --pid-file, --cwd, --env, --user\n");
	fprintf(stderr,
		"  ps/list: --format json, list: -q/--quiet\n");
	fprintf(stderr, "Global options:\n");
	fprintf(stderr, "      --root <dir>     Runtime state root\n");
	fprintf(stderr, "      --log <path>     Accepted for runc CLI compatibility\n");
	fprintf(stderr, "      --log-format <f> Accepted for runc CLI compatibility\n");
	fprintf(stderr, "      --debug          Accepted for runc CLI compatibility\n");
	fprintf(stderr,
		"      --systemd-cgroup Accepted for runc CLI compatibility\n");
	fprintf(stderr, "Environment:\n"
			"  FLUX_RUN_CFG_FILE   Flux run cfg JSON file\n");
}

int main(int argc, char **argv)
{
	char **normalized_argv = NULL;
	int normalized_argc = argc;
	int ret;

	ret = flux_runc_normalize_argv(argc, argv, &normalized_argc,
					 &normalized_argv);
	if (ret < 0)
		return ret;
	if (normalized_argv) {
		argv = normalized_argv;
		argc = normalized_argc;
	}

	if (flux_runc_wants_help(argc, argv)) {
		flux_runc_print_usage();
		ret = argc < 2 ? -1 : 0;
		goto out;
	}

	if (!strcmp(argv[1], "__exec") && !getenv("FLUX_MULTIPROC"))
	{
		ret = flux_launch_bootstrap_multiproc(argv);
		goto out;
	}

	if (!strcmp(argv[1], "__exec"))
	{
		ret = flux_runc_runner_exec(argc >= 3 ? argv[2] : NULL,
					    argc >= 4 ? &argv[3] : NULL);
		goto out;
	}

	ret = flux_runc_dispatch(argc, argv);
	if (ret == -2) {
		FLUX_LOG(FLUX_LOG_ERR, "unsupported flux-runc command '%s'\n",
			 argv[1]);
		flux_runc_print_usage();
	}
	if (ret != 0 && flux_runc_opts.log && flux_runc_opts.log_format &&
	    !strcmp(flux_runc_opts.log_format, "json")) {
		int log_ret = flux_runc_write_runtime_error(flux_runc_opts.log,
							    ret);

		if (log_ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to write runtime error log %s: %s\n",
				 flux_runc_opts.log, strerror(-log_ret));
		}
	}
out:
	free(normalized_argv);
	return ret;
}
