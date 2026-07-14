#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <flux.h>

#include "runc.h"

struct flux_runc_signal_map {
	const char *name;
	int signo;
};

static const struct flux_runc_signal_map flux_runc_signals[] = {
	{ "HUP", SIGHUP },   { "SIGHUP", SIGHUP },   { "INT", SIGINT },
	{ "SIGINT", SIGINT }, { "QUIT", SIGQUIT },   { "SIGQUIT", SIGQUIT },
	{ "KILL", SIGKILL }, { "SIGKILL", SIGKILL }, { "TERM", SIGTERM },
	{ "SIGTERM", SIGTERM }, { "SEGV", SIGSEGV }, { "SIGSEGV", SIGSEGV },
	{ "USR1", SIGUSR1 }, { "SIGUSR1", SIGUSR1 }, { "USR2", SIGUSR2 },
	{ "SIGUSR2", SIGUSR2 },
#ifdef SIGABRT
	{ "ABRT", SIGABRT }, { "SIGABRT", SIGABRT },
#endif
#ifdef SIGALRM
	{ "ALRM", SIGALRM }, { "SIGALRM", SIGALRM },
#endif
#ifdef SIGBUS
	{ "BUS", SIGBUS }, { "SIGBUS", SIGBUS },
#endif
#ifdef SIGCHLD
	{ "CHLD", SIGCHLD }, { "SIGCHLD", SIGCHLD },
#endif
#ifdef SIGCONT
	{ "CONT", SIGCONT }, { "SIGCONT", SIGCONT },
#endif
#ifdef SIGFPE
	{ "FPE", SIGFPE }, { "SIGFPE", SIGFPE },
#endif
#ifdef SIGILL
	{ "ILL", SIGILL }, { "SIGILL", SIGILL },
#endif
#ifdef SIGPIPE
	{ "PIPE", SIGPIPE }, { "SIGPIPE", SIGPIPE },
#endif
#ifdef SIGSTOP
	{ "STOP", SIGSTOP }, { "SIGSTOP", SIGSTOP },
#endif
#ifdef SIGTRAP
	{ "TRAP", SIGTRAP }, { "SIGTRAP", SIGTRAP },
#endif
#ifdef SIGTSTP
	{ "TSTP", SIGTSTP }, { "SIGTSTP", SIGTSTP },
#endif
#ifdef SIGTTIN
	{ "TTIN", SIGTTIN }, { "SIGTTIN", SIGTTIN },
#endif
#ifdef SIGTTOU
	{ "TTOU", SIGTTOU }, { "SIGTTOU", SIGTTOU },
#endif
#ifdef SIGURG
	{ "URG", SIGURG }, { "SIGURG", SIGURG },
#endif
#ifdef SIGWINCH
	{ "WINCH", SIGWINCH }, { "SIGWINCH", SIGWINCH },
#endif
};

enum {
	FLUX_RUNC_OPT_CONSOLE_SOCKET = 1000,
	FLUX_RUNC_OPT_EXEC_PID_FILE,
	FLUX_RUNC_OPT_ALL,
};

int flux_runc_parse_bundle_command(int argc, char **argv,
				   struct flux_runc_bundle_cmd *cmd)
{
	static struct option options[] = {
		{ "bundle", required_argument, 0, 'b' },
		{ "console-socket", required_argument, 0,
		  FLUX_RUNC_OPT_CONSOLE_SOCKET },
		{ "detach", no_argument, 0, 'd' },
		{ "pid-file", required_argument, 0, 'p' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	memset(cmd, 0, sizeof(*cmd));
	cmd->bundle_dir = ".";
	optind = 2;

	while ((opt = getopt_long(argc, argv, "+b:dp:h", options, NULL)) != -1) {
		switch (opt) {
		case 'b':
			cmd->bundle_dir = optarg;
			break;
		case FLUX_RUNC_OPT_CONSOLE_SOCKET:
			cmd->console_socket = optarg;
			break;
		case 'd':
			cmd->detach = true;
			break;
		case 'p':
			cmd->pid_file = optarg;
			break;
		case 'h':
		default:
			return -EINVAL;
		}
	}

	if (argc - optind != 1)
		return -EINVAL;

	cmd->container_id = argv[optind];
	return 0;
}

int flux_runc_parse_simple_command(int argc, char **argv,
				   struct flux_runc_simple_cmd *cmd)
{
	static struct option options[] = {
		{ "bundle", required_argument, 0, 'b' },
		{ "console-socket", required_argument, 0,
		  FLUX_RUNC_OPT_CONSOLE_SOCKET },
		{ "all", no_argument, 0, FLUX_RUNC_OPT_ALL },
		{ "detach", no_argument, 0, 'd' },
		{ "force", no_argument, 0, 'f' },
		{ "pid-file", required_argument, 0, 'p' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	memset(cmd, 0, sizeof(*cmd));
	optind = 2;

	while ((opt = getopt_long(argc, argv, "+b:dfp:h", options, NULL)) != -1) {
		switch (opt) {
		case 'b':
			/* State-bearing commands load the persisted bundle path. */
			break;
		case FLUX_RUNC_OPT_CONSOLE_SOCKET:
			cmd->console_socket = optarg;
			break;
		case FLUX_RUNC_OPT_ALL:
			cmd->all = true;
			break;
		case 'd':
			cmd->detach = true;
			break;
		case 'f':
			cmd->force = true;
			break;
		case 'p':
			cmd->pid_file = optarg;
			break;
		case 'h':
		default:
			return -EINVAL;
		}
	}

	if (argc - optind != 1)
		return -EINVAL;

	cmd->container_id = argv[optind];
	return 0;
}

int flux_runc_parse_kill_command(int argc, char **argv,
				 struct flux_runc_kill_cmd *cmd)
{
	static struct option options[] = {
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	memset(cmd, 0, sizeof(*cmd));
	optind = 2;

	while ((opt = getopt_long(argc, argv, "+h", options, NULL)) != -1) {
		switch (opt) {
		case 'h':
		default:
			return -EINVAL;
		}
	}

	if (argc - optind < 1 || argc - optind > 2)
		return -EINVAL;

	cmd->container_id = argv[optind];
	if (argc - optind == 2)
		cmd->signal_spec = argv[optind + 1];

	return 0;
}

int flux_runc_parse_signal_spec(const char *spec, int *signo)
{
	char *end = NULL;
	long value;
	size_t i;

	if (!spec || !spec[0]) {
		*signo = SIGTERM;
		return 0;
	}

	errno = 0;
	value = strtol(spec, &end, 10);
	if (errno == 0 && end && *end == '\0') {
		if (value <= 0 || value >= NSIG)
			return -EINVAL;
		*signo = (int)value;
		return 0;
	}

	for (i = 0; i < sizeof(flux_runc_signals) / sizeof(flux_runc_signals[0]);
	     i++) {
		if (!strcasecmp(spec, flux_runc_signals[i].name)) {
			*signo = flux_runc_signals[i].signo;
			return 0;
		}
	}

	return -EINVAL;
}

int flux_runc_parse_exec_command(int argc, char **argv,
				 struct flux_runc_exec_cmd *cmd)
{
	static struct option options[] = {
		{ "detach", no_argument, 0, 'd' },
		{ "process", required_argument, 0, 'p' },
		{ "pid-file", required_argument, 0,
		  FLUX_RUNC_OPT_EXEC_PID_FILE },
		{ "cwd", required_argument, 0, 'c' },
		{ "env", required_argument, 0, 'e' },
		{ "user", required_argument, 0, 'u' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	memset(cmd, 0, sizeof(*cmd));
	optind = 2;

	while ((opt = getopt_long(argc, argv, "+dp:c:e:u:h", options, NULL)) !=
	       -1) {
		switch (opt) {
		case 'd':
			cmd->detach = true;
			break;
		case 'p':
			cmd->process_path = optarg;
			break;
		case FLUX_RUNC_OPT_EXEC_PID_FILE:
			cmd->pid_file = optarg;
			break;
		case 'c':
			cmd->cwd = optarg;
			break;
		case 'e': {
			char **env = realloc(cmd->env,
					     sizeof(*cmd->env) *
						     (size_t)(cmd->env_count + 1));
			if (!env)
				return -ENOMEM;
			cmd->env = env;
			cmd->env[cmd->env_count++] = optarg;
			break;
		}
		case 'u':
			cmd->user_spec = optarg;
			break;
		case 'h':
		default:
			return -EINVAL;
		}
	}

	if (argc - optind < 1) {
		free(cmd->env);
		cmd->env = NULL;
		cmd->env_count = 0;
		return -EINVAL;
	}

	cmd->container_id = argv[optind++];
	cmd->argc = argc - optind;
	cmd->argv = cmd->argc > 0 ? &argv[optind] : NULL;

	if (!!cmd->process_path == !!cmd->argc) {
		free(cmd->env);
		cmd->env = NULL;
		cmd->env_count = 0;
		return -EINVAL;
	}

	return 0;
}

int flux_runc_dispatch(int argc, char **argv)
{
	if (argc < 2)
		return -2;

	if (!strcmp(argv[1], "create"))
		return flux_runc_cmd_create(argc, argv);
	if (!strcmp(argv[1], "state"))
		return flux_runc_cmd_state(argc, argv);
	if (!strcmp(argv[1], "start"))
		return flux_runc_cmd_start(argc, argv);
	if (!strcmp(argv[1], "delete"))
		return flux_runc_cmd_delete(argc, argv);
	if (!strcmp(argv[1], "run"))
		return flux_runc_cmd_run(argc, argv);
	if (!strcmp(argv[1], "kill"))
		return flux_runc_cmd_kill(argc, argv);
	if (!strcmp(argv[1], "exec"))
		return flux_runc_cmd_exec(argc, argv);
	if (!strcmp(argv[1], "features"))
		return flux_runc_cmd_features(argc, argv);
	if (!strcmp(argv[1], "ps"))
		return flux_runc_cmd_ps(argc, argv);
	if (!strcmp(argv[1], "list"))
		return flux_runc_cmd_list(argc, argv);

	return -2;
}
