#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define FLUX_FMT "signal: "

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include <flux.h>
#include <flux/mpk.h>

#include "kmod.h"

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
static __thread void *thread_signal_stack_base;
static __thread size_t thread_signal_stack_slot_size;

static __attribute__((no_stack_protector)) inline unsigned long
flux_signal_read_fsbase(void)
{
	unsigned long fsbase;

	asm volatile("rdfsbase %0" : "=r"(fsbase));
	return fsbase;
}

static __attribute__((no_stack_protector)) inline void
flux_signal_write_fsbase(unsigned long fsbase)
{
	asm volatile("wrfsbase %0" : : "r"(fsbase) : "memory");
}

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

extern void flux_host_signal_entry(int signum, siginfo_t *info, void *ucontext);

void __attribute__((no_stack_protector))
flux_host_signal_handler(int signum, siginfo_t *info, void *ucontext,
			 unsigned int interrupted_pkru, uint64_t signal_cookie,
			 unsigned long signal_host_fsbase)
{
	unsigned long interrupted_fsbase = flux_signal_read_fsbase();
	int flux_cpu = -1;
	int logical_cpu = -1;
	int captured_uif = 0;
	int metadata_tagged;
	int handled;

	/*
	 * R8/R9 are private live handler-entry arguments, installed only after
	 * native setup copied the interrupted GPRs into the Linux signal frame.
	 * The strong R8 tag proves the provenance of the R9 host FSBASE.
	 */
	metadata_tagged =
		(signal_cookie & FLUX_SIGNAL_ENTRY_COOKIE_TAG_MASK) ==
		FLUX_SIGNAL_ENTRY_COOKIE_TAG;
	if (metadata_tagged && signal_host_fsbase)
		flux_signal_write_fsbase(signal_host_fsbase);
	if (metadata_tagged && signal_host_fsbase &&
	    (signal_cookie & FLUX_SIGNAL_ENTRY_COOKIE_VALID)) {
		logical_cpu =
			(int)(signal_cookie & FLUX_SIGNAL_ENTRY_COOKIE_CPU_MASK);
		captured_uif =
			!!(signal_cookie & FLUX_SIGNAL_ENTRY_COOKIE_UIF);
	}
	if (logical_cpu >= 0 && logical_cpu < flux_env.nr_cpus)
		flux_cpu = logical_cpu;
	else
		captured_uif = 0;
	FLUX_LOG(FLUX_LOG_DEBUG, "Caught signal %d (si_code=%d)\n", signum,
		 info ? info->si_code : 0);
	handled = flux_signal_handler(signum, info, ucontext, interrupted_pkru,
				      flux_cpu, captured_uif);
	if (handled < 0) {
		flux_signal_write_fsbase(interrupted_fsbase);
		_exit(128 + signum);
	}
#ifdef CONFIG_FLUX_MPK
	if (signum == SIGILL && !handled)
		FLUX_LOG(FLUX_LOG_ERR,
			 "unmatched application SIGILL at %p ip=%p code=%d\n",
			 info ? info->si_addr : NULL,
			 ucontext ? (void *)(uintptr_t)((ucontext_t *)ucontext)
				->uc_mcontext.gregs[REG_RIP] : NULL,
			 info ? info->si_code : 0);
#endif
	flux_signal_write_fsbase(interrupted_fsbase);
}

static int add_sigaction(int signum)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = flux_host_signal_entry;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	/*
	 * Flux uses synchronous host memory faults as its Flux page-fault entry.
	 * Resolving one fault can itself touch another not-yet-aliased Flux page,
	 * especially while several CPUs update COW aliases in the same host mm.
	 * Without SA_NODEFER that second SIGSEGV/SIGBUS stays blocked and Linux
	 * terminates the process instead of entering the Flux fault handler again.
	 */
	if (signum == SIGSEGV || signum == SIGBUS)
		sa.sa_flags |= SA_NODEFER;
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

	/*
	 * Synchronous CPU-fault signals are part of Flux's execution bridge,
	 * rather than optional Flux signal forwarding.  In particular, the
	 * SKAS MM path needs SIGSEGV/SIGBUS to resolve missing host aliases and
	 * the uaccess fixup path needs them even when the runtime config has no
	 * signal list.  Keep these handlers installed for every runtime.
	 */
	{
		static const int sync_fault_sigs[] = {
			SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS,
		};
		size_t k;

		for (k = 0; k < sizeof(sync_fault_sigs) /
				    sizeof(sync_fault_sigs[0]); k++) {
			if (append_handled_signal(sync_fault_sigs[k]) < 0)
				return -1;
		}
	}

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
	 * explicitly unblock in flux_signal_enable_percpu().
	 */

	sigemptyset(&handled_set);
	for (i = 0; i < handled_signals_num; i++) {
		/*
		 * SIGSYS is synchronous and thread-directed. Keep it unblocked so
		 * the installed handler can forward a genuine Flux SIGSYS.
		 */
		if (handled_signals[i] != SIGSYS)
			sigaddset(&handled_set, handled_signals[i]);
	}

	if (pthread_sigmask(SIG_BLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "pthread_sigmask(SIG_BLOCK) failed\n");
		return -1;
	}

	return 0;
}

static int flux_signal_init_altstack(void)
{
	const size_t nr_slots = FLUX_SIGNAL_STACK_SLOTS + 1;
	size_t stack_size = SIGSTKSZ;
	size_t map_size;
	size_t arena_size;
	long page_size;
	void *map;
	void *stack;
	stack_t altstack;

#ifdef _SC_SIGSTKSZ
	long dynamic_stack_size = sysconf(_SC_SIGSTKSZ);

	if (dynamic_stack_size > 0 && (size_t)dynamic_stack_size > stack_size)
		stack_size = dynamic_stack_size;
#endif


	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 ||
	    stack_size > SIZE_MAX - ((size_t)page_size - 1))
		return -1;
	stack_size = (stack_size + (size_t)page_size - 1) &
		     ~((size_t)page_size - 1);
	if (stack_size > (SIZE_MAX - 2 * (size_t)page_size) / nr_slots)
		return -1;
	arena_size = stack_size * nr_slots;
	map_size = arena_size + 2 * (size_t)page_size;

	map = mmap(NULL, map_size, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (map == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "mmap for signal stack failed\n");
		return -1;
	}
	stack = map + page_size;
	if (mprotect(stack, arena_size, PROT_READ | PROT_WRITE) < 0) {
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
	/* Slot zero covers the short interval before kmod setup completes. */
	thread_signal_stack_base = stack + stack_size;
	thread_signal_stack_slot_size = stack_size;

	return 0;
}

int flux_signal_init_percpu(void)
{
	thread_signal_stack_base = NULL;
	thread_signal_stack_slot_size = 0;
	return flux_signal_init_altstack();
}

void *flux_signal_stack_base(void)
{
	return thread_signal_stack_base;
}

size_t flux_signal_stack_slot_size(void)
{
	return thread_signal_stack_slot_size;
}

int flux_signal_enable_percpu(void)
{
	/*
	 * Keep inherited handled signals blocked until the altstack and kmod
	 * handler-entry metadata are both ready on this per-CPU worker.
	 */
	if (pthread_sigmask(SIG_UNBLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "pthread_sigmask(SIG_UNBLOCK) failed\n");
		return -1;
	}
	return 0;
}

int flux_signal_restore_defaults(void)
{
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
}

int flux_signal_exit_percpu(void)
{
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
	thread_signal_stack_base = NULL;
	thread_signal_stack_slot_size = 0;
	return 0;
}

int flux_signal_block_current(void)
{
	if (pthread_sigmask(SIG_BLOCK, &handled_set, NULL) != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "pthread_sigmask(SIG_BLOCK) failed\n");
		return -1;
	}

	return 0;
}
