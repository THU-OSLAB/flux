#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <flux.h>
#include <flux/runc.h>

#include "host/kmod.h"

extern char **environ;

static const struct flux_launch_spec *flux_launch;
static volatile sig_atomic_t flux_launch_forward_pid;
static int flux_launch_forwarded_signals[NSIG];
static size_t flux_launch_forwarded_signals_num;

static int flux_launch_parse_envp(struct flux_launch_spec *spec, int argc,
				  char **argv)
{
	char **new_envp = NULL;
	int envc = 0;
	int i;

	for (i = 0; i < argc; i++) {
		char **tmp;

		if (strchr(argv[i], '=') == NULL)
			break;

		tmp = realloc(new_envp, sizeof(char *) * (envc + 2));
		if (!tmp)
			goto out_free;
		new_envp = tmp;

		new_envp[envc] = strdup(argv[i]);
		if (!new_envp[envc])
			goto out_free;

		envc++;
		new_envp[envc] = NULL;
		optind++;
	}

	if (!new_envp) {
		new_envp = calloc(1, sizeof(char *));
		if (!new_envp)
			return -1;
	}

	spec->extra_envp = new_envp;
	spec->owns_extra_envp = true;
	return 0;

out_free:
	for (i = 0; i < envc; i++)
		free(new_envp[i]);
	free(new_envp);
	return -1;
}

static int flux_launch_get_self_path(char *buf, size_t size)
{
	ssize_t len;

	len = readlink("/proc/self/exe", buf, size - 1);
	if (len < 0) {
		perror("readlink(/proc/self/exe)");
		return -1;
	}
	if ((size_t)len >= size - 1) {
		fprintf(stderr, "self executable path is too long\n");
		return -1;
	}

	buf[len] = '\0';
	return 0;
}

static int flux_launch_multiproc_exec_self(char *argv[])
{
	char self_path[FLUX_PATH_MAX];
	int fd;

	if (flux_launch_get_self_path(self_path, sizeof(self_path)) < 0)
		return -1;

	fd = open(FLUX_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open flux dev");
		return -1;
	}

	if (ioctl(fd, FLUX_DEV_IO_START_MAP_SHARED) < 0) {
		perror("ioctl start map shared");
		close(fd);
		return -1;
	}

	if (ioctl(fd, FLUX_DEV_IO_EXECVE,
		  (unsigned long)&(struct flux_execve_args){
			  .filename = self_path,
			  .argv = argv,
			  .envp = environ,
		  }) < 0) {
		perror("ioctl execve");
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static int flux_launch_wait_child(pid_t pid)
{
	int status;

	while (waitpid(pid, &status, 0) < 0) {
		if (errno == FLUX_EINTR)
			continue;
		perror("waitpid");
		return EXIT_FAILURE;
	}

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);

	return EXIT_FAILURE;
}

struct flux_launch_signal_map {
	const char *name;
	int signo;
};

static const struct flux_launch_signal_map flux_launch_signal_map[] = {
	{ "HUP", SIGHUP },     { "SIGHUP", SIGHUP },
	{ "INT", SIGINT },     { "SIGINT", SIGINT },
	{ "QUIT", SIGQUIT },   { "SIGQUIT", SIGQUIT },
	{ "SEGV", SIGSEGV },   { "SIGSEGV", SIGSEGV },
	{ "TERM", SIGTERM },   { "SIGTERM", SIGTERM },
	{ "USR1", SIGUSR1 },   { "SIGUSR1", SIGUSR1 },
	{ "USR2", SIGUSR2 },   { "SIGUSR2", SIGUSR2 },
};

static int flux_launch_parse_signal_spec(const char *spec, int *signo)
{
	char *end = NULL;
	long value;
	size_t i;

	if (!spec || !spec[0])
		return -EINVAL;

	errno = 0;
	value = strtol(spec, &end, 10);
	if (errno == 0 && end && *end == '\0') {
		if (value <= 0 || value >= NSIG)
			return -EINVAL;
		*signo = (int)value;
		return 0;
	}

	for (i = 0; i < sizeof(flux_launch_signal_map) /
			    sizeof(flux_launch_signal_map[0]);
	     i++) {
		if (!strcasecmp(spec, flux_launch_signal_map[i].name)) {
			*signo = flux_launch_signal_map[i].signo;
			return 0;
		}
	}

	return -EINVAL;
}

static void flux_launch_forward_signal_handler(int signum, siginfo_t *info,
					       void *ucontext)
{
	pid_t child = flux_launch_forward_pid;
	int saved_errno = errno;

	(void)ucontext;

	if (child > 0) {
		if (info && info->si_code == SI_QUEUE) {
			if (sigqueue(child, signum, info->si_value) < 0)
				kill(child, signum);
		} else {
			kill(child, signum);
		}
	}

	errno = saved_errno;
}

static void flux_launch_reset_forwarding_signals(void)
{
	struct sigaction sa = { 0 };
	size_t i;

	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < flux_launch_forwarded_signals_num; i++)
		sigaction(flux_launch_forwarded_signals[i], &sa, NULL);

	flux_launch_forward_pid = 0;
	flux_launch_forwarded_signals_num = 0;
}

static int flux_launch_install_forwarding_signals(pid_t child_pid)
{
	struct sigaction sa = { 0 };
	char *copy = NULL;
	char *cursor;
	char *token;
	const char *env;

	env = getenv(FLUX_SIGNAL_BRIDGE_ENV);
	if (!env || !env[0]) {
		flux_launch_forward_pid = child_pid;
		return 0;
	}

	copy = strdup(env);
	if (!copy)
		return -ENOMEM;

	flux_launch_forward_pid = child_pid;
	sa.sa_sigaction = flux_launch_forward_signal_handler;
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sigfillset(&sa.sa_mask);

	cursor = copy;
	while ((token = strsep(&cursor, ", \t\r\n")) != NULL) {
		int signo;

		if (!token[0])
			continue;
		if (flux_launch_parse_signal_spec(token, &signo) < 0) {
			free(copy);
			flux_launch_reset_forwarding_signals();
			return -EINVAL;
		}
		if (sigaction(signo, &sa, NULL) < 0) {
			int ret = -errno;

			free(copy);
			flux_launch_reset_forwarding_signals();
			return ret;
		}
		if (flux_launch_forwarded_signals_num >=
		    sizeof(flux_launch_forwarded_signals) /
			    sizeof(flux_launch_forwarded_signals[0])) {
			free(copy);
			flux_launch_reset_forwarding_signals();
			return -E2BIG;
		}
		flux_launch_forwarded_signals[flux_launch_forwarded_signals_num++] =
			signo;
	}

	free(copy);
	return 0;
}

bool flux_launch_should_bootstrap_multiproc(int argc, char **argv)
{
	int i;

	if (getenv("FLUX_MULTIPROC"))
		return false;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--"))
			break;

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help"))
			return false;

		if (!strcmp(arg, "--multiproc"))
			return true;

		if (arg[0] != '-' || arg[1] == '\0')
			break;

		if (!strcmp(arg, "-c") || !strcmp(arg, "-b")) {
			i++;
			continue;
		}

		if (!strncmp(arg, "--", 2) && strchr(arg, '=') == NULL) {
			i++;
			continue;
		}
	}

	/* Every LibOS process uses an independent host execution mm. */
	return true;
}

int flux_launch_bootstrap_multiproc(char *argv[])
{
	pid_t pid;
	int ret;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return EXIT_FAILURE;
	}

	if (pid == 0) {
		if (setenv("FLUX_MULTIPROC", "1", 1) < 0) {
			perror("setenv(FLUX_MULTIPROC)");
			_exit(EXIT_FAILURE);
		}

		if (flux_launch_multiproc_exec_self(argv) < 0)
			_exit(EXIT_FAILURE);

		_exit(EXIT_FAILURE);
	}

	ret = flux_launch_install_forwarding_signals(pid);
	if (ret < 0) {
		fprintf(stderr, "failed to install signal forwarding: %s\n",
			strerror(-ret));
		kill(pid, SIGKILL);
		(void)flux_launch_wait_child(pid);
		return EXIT_FAILURE;
	}

	ret = flux_launch_wait_child(pid);
	flux_launch_reset_forwarding_signals();
	return ret;
}

static int flux_launch_prepare_runc_bundle(struct flux_launch_spec *spec)
{
	const struct flux_oci_cfg *oci_cfg;

	if (flux_runc_load_bundle() < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to load container bundle\n");
		return -1;
	}

	oci_cfg = flux_oci_cfg_get();
	if (!oci_cfg || !oci_cfg->argv || oci_cfg->argc <= 0)
		return -1;

	spec->uses_oci_cfg = true;
	spec->argv = (char **)oci_cfg->argv;
	spec->argc = oci_cfg->argc;
	spec->filename = oci_cfg->exec_path ? oci_cfg->exec_path :
					      oci_cfg->argv[0];
	return 0;
}

int flux_launch_prepare_flux_cli(struct flux_launch_spec *spec, int argc,
				 char **argv)
{
	int ret;

	memset(spec, 0, sizeof(*spec));

	ret = flux_run_cfg_parse_args(argc, argv);
	if (ret > 0)
		return ret;
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to parse run cfg arguments\n");
		return -1;
	}

	if (flux_launch_parse_envp(spec, argc - optind, &argv[optind]) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to parse environment variables\n");
		return -1;
	}

	if (argc - optind <= 0) {
		FLUX_LOG(FLUX_LOG_ERR, "no arguments provided\n");
		return -1;
	}

	spec->kind = FLUX_LAUNCH_NATIVE;
	spec->argv = &argv[optind];
	spec->argc = argc - optind;
	spec->filename = spec->argv[0];
	return 0;
}

int flux_launch_ensure_run_cfg_path(void)
{
	char self_path[FLUX_PATH_MAX];
	char *slash;

	if (getenv("FLUX_RUN_CFG_FILE"))
		return 0;

	if (flux_launch_get_self_path(self_path, sizeof(self_path)) < 0)
		return -1;

	slash = strrchr(self_path, '/');
	if (!slash)
		return -1;
	*slash = '\0';

	slash = strrchr(self_path, '/');
	if (!slash)
		return -1;
	*slash = '\0';

	if (snprintf(self_path + strlen(self_path),
		     sizeof(self_path) - strlen(self_path),
		     "/configs/config.json") >=
	    (int)(sizeof(self_path) - strlen(self_path)))
		return -1;

	if (setenv("FLUX_RUN_CFG_FILE", self_path, 0) < 0)
		return -1;

	return 0;
}

static int flux_launch_parse_runc_run_args(int argc, char **argv)
{
	static struct option options[] = {
		{ "bundle", required_argument, 0, 'b' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	optind = 1;

	while ((opt = getopt_long(argc, argv, "+b:h", options, NULL)) != -1) {
		switch (opt) {
		case 'b':
			if (flux_runc_set_bundle(optarg) < 0) {
				FLUX_LOG(
					FLUX_LOG_ERR,
					"failed to resolve container bundle %s\n",
					optarg);
				return -1;
			}
			break;
		case 'h':
			return -1;
		default:
			FLUX_LOG(FLUX_LOG_ERR,
				 "unsupported flux-runc run option\n");
			return -1;
		}
	}

	return 0;
}

int flux_launch_prepare_runc_run(struct flux_launch_spec *spec, int argc,
				 char **argv)
{
	int positional;

	memset(spec, 0, sizeof(*spec));

	if (flux_launch_parse_runc_run_args(argc, argv) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to parse flux-runc run arguments\n");
		return -1;
	}

	if (flux_launch_ensure_run_cfg_path() < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to resolve default run cfg path\n");
		return -1;
	}

	if (!flux_runc_bundle_enabled() && flux_runc_set_bundle(".") < 0) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"failed to resolve default container bundle from cwd\n");
		return -1;
	}

	positional = argc - optind;
	if (positional != 1) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "flux-runc run requires exactly one container id\n");
		return -1;
	}

	spec->kind = FLUX_LAUNCH_RUNC_RUN;
	return flux_launch_prepare_runc_container(
		spec, argv[optind], flux_oci_cfg_get()->bundle_dir,
		flux_oci_cfg_get()->config_path);
}

int flux_launch_prepare_runc_container(struct flux_launch_spec *spec,
				       const char *container_id,
				       const char *bundle_dir,
				       const char *config_path)
{
	memset(spec, 0, sizeof(*spec));

	if (flux_launch_ensure_run_cfg_path() < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to resolve default run cfg path\n");
		return -1;
	}

	if (flux_runc_set_bundle_config(bundle_dir, config_path) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to resolve container bundle %s\n", bundle_dir);
		return -1;
	}

	spec->kind = FLUX_LAUNCH_RUNC_RUN;
	spec->container_id = strdup(container_id);
	if (!spec->container_id)
		return -1;

	return flux_launch_prepare_runc_bundle(spec);
}

int flux_launch_run(struct flux_launch_spec *spec)
{
	int ret;

	flux_launch = spec;
	flux_extra_envp = spec->extra_envp;
	ret = flux_stack_init(flux_env_init, spec->argc, spec->argv);
	flux_extra_envp = NULL;
	flux_launch = NULL;
	flux_runc_unload();
	return ret;
}

void flux_launch_cleanup(struct flux_launch_spec *spec)
{
	int i;

	if (!spec)
		return;

	if (spec->owns_extra_envp && spec->extra_envp) {
		for (i = 0; spec->extra_envp[i]; i++)
			free(spec->extra_envp[i]);
		free(spec->extra_envp);
	}

	free(spec->container_id);
	memset(spec, 0, sizeof(*spec));
	flux_runc_unload();
}

const struct flux_launch_spec *flux_launch_get(void)
{
	return flux_launch;
}
