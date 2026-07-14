#define FLUX_FMT "signal: "

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <flux.h>

#ifdef CONFIG_FLUX_UINTR
struct flux_signal_name_map {
	const char *name;
	int signo;
};

static const struct flux_signal_name_map signal_name_map[] = {
	{ "SIGHUP", SIGHUP },	    { "SIGINT", SIGINT },
	{ "SIGQUIT", SIGQUIT },	    { "SIGILL", SIGILL },
	{ "SIGTRAP", SIGTRAP },	    { "SIGABRT", SIGABRT },
	{ "SIGBUS", SIGBUS },	    { "SIGFPE", SIGFPE },
	{ "SIGKILL", SIGKILL },	    { "SIGUSR1", SIGUSR1 },
	{ "SIGSEGV", SIGSEGV },	    { "SIGUSR2", SIGUSR2 },
	{ "SIGPIPE", SIGPIPE },	    { "SIGALRM", SIGALRM },
	{ "SIGTERM", SIGTERM },
#ifdef SIGSTKFLT
	{ "SIGSTKFLT", SIGSTKFLT },
#endif
	{ "SIGCHLD", SIGCHLD },	    { "SIGCONT", SIGCONT },
	{ "SIGSTOP", SIGSTOP },	    { "SIGTSTP", SIGTSTP },
	{ "SIGTTIN", SIGTTIN },	    { "SIGTTOU", SIGTTOU },
	{ "SIGURG", SIGURG },	    { "SIGXCPU", SIGXCPU },
	{ "SIGXFSZ", SIGXFSZ },	    { "SIGVTALRM", SIGVTALRM },
	{ "SIGPROF", SIGPROF },	    { "SIGWINCH", SIGWINCH },
	{ "SIGIO", SIGIO },	    { "SIGPWR", SIGPWR },
	{ "SIGSYS", SIGSYS },
};

static int handled_signals[NSIG];
static size_t handled_signals_num;
static sigset_t handled_set;
static __thread void *thread_altstack_map;
static __thread size_t thread_altstack_map_size;

static int parse_signal_number(const char *name, int *signo)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(name, &end, 10);
	if (errno || !end || *end != '\0')
		return -1;
	if (value <= 0 || value >= NSIG)
		return -1;

	*signo = (int)value;
	return 0;
}

static int parse_signal_name(const char *name, int *signo)
{
	size_t i;
	char with_prefix[32];

	for (i = 0; i < sizeof(signal_name_map) / sizeof(signal_name_map[0]);
	     i++) {
		if (strcasecmp(name, signal_name_map[i].name) == 0) {
			*signo = signal_name_map[i].signo;
			return 0;
		}
	}

	if (strncasecmp(name, "SIG", 3) != 0) {
		int ret = snprintf(with_prefix, sizeof(with_prefix), "SIG%s",
				   name);
		if (ret > 0 && ret < (int)sizeof(with_prefix))
			return parse_signal_name(with_prefix, signo);
	}

	return -1;
}

static int parse_signal_spec(const char *name, int *signo)
{
	if (!name || !*name)
		return -1;

	if (parse_signal_name(name, signo) == 0)
		return 0;

	return parse_signal_number(name, signo);
}

static int append_handled_signal(int signo)
{
	size_t i;

	if (signo == SIGKILL || signo == SIGSTOP)
		return -1;

	for (i = 0; i < handled_signals_num; i++) {
		if (handled_signals[i] == signo)
			return 0;
	}

	if (handled_signals_num >=
	    sizeof(handled_signals) / sizeof(handled_signals[0]))
		return -1;

	handled_signals[handled_signals_num++] = signo;
	return 0;
}

static int load_handled_signals_from_config(void)
{
	int i;
	int signo;

	handled_signals_num = 0;

	if (!run_cfg || !run_cfg->signals || run_cfg->signals_num <= 0)
		return 0;

	for (i = 0; i < run_cfg->signals_num; i++) {
		if (parse_signal_spec(run_cfg->signals[i], &signo) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "invalid signal in run cfg: %s\n",
				 run_cfg->signals[i]);
			return -1;
		}

		if (append_handled_signal(signo) < 0)
			return -1;
	}

	return 0;
}

static int load_handled_signals_from_env(void)
{
	char *copy = NULL;
	char *cursor;
	char *token;
	const char *env;
	int signo;

	env = getenv(FLUX_SIGNAL_BRIDGE_ENV);
	if (!env || !env[0])
		return 0;

	copy = strdup(env);
	if (!copy)
		return -1;

	cursor = copy;
	while ((token = strsep(&cursor, ", \t\r\n")) != NULL) {
		if (!token[0])
			continue;

		if (parse_signal_spec(token, &signo) < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "invalid signal in %s: %s\n",
				 FLUX_SIGNAL_BRIDGE_ENV, token);
			free(copy);
			return -1;
		}

		if (append_handled_signal(signo) < 0) {
			free(copy);
			return -1;
		}
	}

	free(copy);
	return 0;
}

#ifdef CONFIG_FLUX_MPK
extern void flux_host_signal_entry(int signum, siginfo_t *info,
				   void *ucontext);

void flux_host_signal_handler(int signum, siginfo_t *info, void *ucontext,
			      unsigned int interrupted_pkru)
#else
static void signal_handler(int signum, siginfo_t *info, void *ucontext)
#endif
{
	FLUX_LOG(FLUX_LOG_DEBUG, "Caught signal %d (si_code=%d)\n", signum,
		 info ? info->si_code : 0);
#ifdef CONFIG_FLUX_MPK
	flux_signal_handler(signum, info, ucontext, interrupted_pkru);
#else
	flux_signal_handler(signum, info, ucontext, 0);
#endif
}

static int add_sigaction(int signum)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
#ifdef CONFIG_FLUX_MPK
	sa.sa_sigaction = flux_host_signal_entry;
#else
	sa.sa_sigaction = signal_handler;
#endif
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigfillset(&sa.sa_mask);
	sigdelset(&sa.sa_mask, signum);
	if (sigaction(signum, &sa, NULL) < 0)
		return -1;

	return 0;
}

int flux_signal_init(void)
{
	size_t i;

	if (load_handled_signals_from_config() < 0)
		return -1;

	if (load_handled_signals_from_env() < 0)
		return -1;

	for (i = 0; i < handled_signals_num; i++) {
		if (add_sigaction(handled_signals[i]) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "sigaction install failed for signal %d\n",
				 handled_signals[i]);
			return -1;
		}
	}

	/*
	 * Block all handled signals in the bootstrap thread. Newly created
	 * threads inherit this mask, then each flux_percpu_entry thread will
	 * explicitly unblock in flux_signal_init_percpu().
	 */

	sigemptyset(&handled_set);
	for (i = 0; i < handled_signals_num; i++)
		sigaddset(&handled_set, handled_signals[i]);

	if (pthread_sigmask(SIG_BLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "pthread_sigmask(SIG_BLOCK) failed\n");
		return -1;
	}

	return 0;
}

static int flux_signal_init_altstack(void)
{
	size_t stack_size = SIGSTKSZ;
	size_t map_size;
	long page_size;
#ifndef CONFIG_FLUX_MPK
	stack_t current;
#endif
	void *map;
	void *stack;
	stack_t altstack;

#ifdef _SC_SIGSTKSZ
	long dynamic_stack_size = sysconf(_SC_SIGSTKSZ);

	if (dynamic_stack_size > 0 && (size_t)dynamic_stack_size > stack_size)
		stack_size = dynamic_stack_size;
#endif

#ifndef CONFIG_FLUX_MPK
	if (sigaltstack(NULL, &current) == 0 &&
	    !(current.ss_flags & SS_DISABLE) && current.ss_size >= SIGSTKSZ) {
		thread_altstack_map = NULL;
		thread_altstack_map_size = 0;
		return 0;
	}
#endif

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || stack_size > SIZE_MAX - 2 * (size_t)page_size)
		return -1;
	map_size = stack_size + 2 * (size_t)page_size;

	map = mmap(NULL, map_size, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (map == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "mmap for signal stack failed\n");
		return -1;
	}
	stack = map + page_size;
	if (mprotect(stack, stack_size, PROT_READ | PROT_WRITE) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "mprotect for signal stack failed\n");
		munmap(map, map_size);
		return -1;
	}

	altstack.ss_sp = stack;
	altstack.ss_size = stack_size;
	altstack.ss_flags = 0;
	if (sigaltstack(&altstack, NULL) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "sigaltstack setup failed\n");
		munmap(map, map_size);
		return -1;
	}

	thread_altstack_map = map;
	thread_altstack_map_size = map_size;

	return 0;
}
#else
int flux_signal_init(void)
{
	return 0;
}
#endif /* CONFIG_FLUX_UINTR */

int flux_signal_init_percpu(void)
{
#ifdef CONFIG_FLUX_UINTR
	if (flux_signal_init_altstack() < 0)
		return -1;

	/*
	 * Only percpu worker threads should receive handled signals.
	 * This makes delivery happen in flux_percpu_entry context.
	 */
	if (pthread_sigmask(SIG_UNBLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "pthread_sigmask(SIG_UNBLOCK) failed\n");
		return -1;
	}

	return 0;
#else
	return 0;
#endif /* CONFIG_FLUX_UINTR */
}

int flux_signal_restore_defaults(void)
{
#ifdef CONFIG_FLUX_UINTR
	struct sigaction sa;
	size_t i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < handled_signals_num; i++) {
		if (sigaction(handled_signals[i], &sa, NULL) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "sigaction(SIG_DFL) failed for signal %d\n",
				 handled_signals[i]);
			return -1;
		}
	}

	return 0;
#else
	return 0;
#endif /* CONFIG_FLUX_UINTR */
}

int flux_signal_exit_percpu(void)
{
#ifdef CONFIG_FLUX_UINTR
	stack_t disable = { 0 };

	if (pthread_sigmask(SIG_BLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"pthread_sigmask(SIG_BLOCK) failed during percpu exit\n");
		return -1;
	}

	if (!thread_altstack_map || !thread_altstack_map_size)
		return 0;

	disable.ss_flags = SS_DISABLE;
	if (sigaltstack(&disable, NULL) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "sigaltstack disable failed during percpu exit\n");
		return -1;
	}

	if (munmap(thread_altstack_map, thread_altstack_map_size) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "munmap signal stack failed during percpu exit\n");
		return -1;
	}

	thread_altstack_map = NULL;
	thread_altstack_map_size = 0;
	return 0;
#else
	return 0;
#endif /* CONFIG_FLUX_UINTR */
}

int flux_signal_block_current(void)
{
#ifdef CONFIG_FLUX_UINTR
	if (pthread_sigmask(SIG_BLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "pthread_sigmask(SIG_BLOCK) failed\n");
		return -1;
	}

	return 0;
#else
	return 0;
#endif /* CONFIG_FLUX_UINTR */
}
