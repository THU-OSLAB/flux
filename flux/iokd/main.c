#define _GNU_SOURCE
#define FLUX_FMT "iokd: "

#include <errno.h>
#include <execinfo.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include <utils/log.h>

#include "iokd.h"

struct flux_iokd flux_iokd = {
	.listen_fd = -1,
	.cpu_lock = PTHREAD_MUTEX_INITIALIZER,
	.control_lock = PTHREAD_MUTEX_INITIALIZER,
	.control_cond = PTHREAD_COND_INITIALIZER,
	.control_cpu = -1,
};
static volatile sig_atomic_t flux_iokd_shutdown_pending;

static void flux_iokd_dump_backtrace(void)
{
	void *pcs[32];
	int nr;

	nr = backtrace(pcs, (int)(sizeof(pcs) / sizeof(pcs[0])));
	if (nr > 0)
		backtrace_symbols_fd(pcs, nr, STDERR_FILENO);
}

static int flux_iokd_bind_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);

	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		return -1;

	while (sched_getcpu() != cpu)
		sched_yield();

	return 0;
}

bool flux_iokd_shutdown_requested(void)
{
	return flux_iokd_shutdown_pending != 0;
}

void flux_iokd_request_shutdown(void)
{
	flux_iokd_shutdown_pending = 1;
}

void flux_iokd_request_client_exit(pid_t pid, int sig)
{
	if (pid > 0)
		kill(pid, sig);
}

void flux_iokd_request_all_clients_exit(int sig)
{
	int i;

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		pid_t pid = atomic_load_relaxed(&flux_iokd.client_pids[i]);

		if (pid > 0)
			flux_iokd_request_client_exit(pid, sig);
	}
}

void flux_iokd_retire_all_clients(void)
{
	int i;

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		struct flux_iokd_client *client = flux_iokd_client_load(i);

		if (!client)
			continue;
		flux_iokd_client_retire(client);
	}
}

static void flux_iokd_signal_handler(int signum)
{
	int saved_errno = errno;

	FLUX_LOG(FLUX_LOG_WARN, "received signal %d, requesting shutdown\n",
		 signum);
	flux_iokd_request_shutdown();
	errno = saved_errno;
}

static void flux_iokd_fault_handler(int signum, siginfo_t *info, void *ctx)
{
	int saved_errno = errno;
#if defined(__x86_64__)
	ucontext_t *uc = ctx;
	unsigned long long rip =
		(unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
	unsigned long long rsp =
		(unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
#endif

#if defined(__x86_64__)
	FLUX_LOG(FLUX_LOG_ERR,
		 "fatal signal %d code=%d addr=%p rip=%#llx rsp=%#llx\n",
		 signum, info ? info->si_code : 0, info ? info->si_addr : NULL,
		 rip, rsp);
#else
	FLUX_LOG(FLUX_LOG_ERR, "fatal signal %d code=%d addr=%p\n", signum,
		 info ? info->si_code : 0, info ? info->si_addr : NULL);
#endif
	flux_iokd_dump_backtrace();
	_exit(128 + signum);
	errno = saved_errno;
}

static int flux_iokd_install_signal_handlers(void)
{
	static const int signals[] = { SIGINT, SIGTERM };
	static const int faults[] = { SIGSEGV, SIGILL, SIGBUS, SIGABRT };
	struct sigaction sa;
	struct sigaction fault_sa;
	size_t i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = flux_iokd_signal_handler;
	sigfillset(&sa.sa_mask);

	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigaction(signals[i], &sa, NULL) < 0)
			return -errno;
	}

	memset(&fault_sa, 0, sizeof(fault_sa));
	fault_sa.sa_sigaction = flux_iokd_fault_handler;
	fault_sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigfillset(&fault_sa.sa_mask);

	for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		if (sigaction(faults[i], &fault_sa, NULL) < 0)
			return -errno;
	}

	return 0;
}

int main(int argc, char **argv)
{
	bool shm_mapped = false;
	bool control_inited = false;
	bool sender_inited = false;
	int ret;

	ret = flux_iokd_config_load(argc, argv);
	if (ret < 0)
		return ret;

	ret = flux_iokd_install_signal_handlers();
	if (ret < 0)
		goto out_cfg;

	ret = flux_iokd_kmod_map_shm();
	if (ret < 0)
		goto out_cfg;
	shm_mapped = true;

	ret = flux_iokd_cpu_allocator_init();
	if (ret < 0)
		goto out_shm;

#ifdef CONFIG_FLUX_FNET
	ret = flux_iokd_fnet_init();
	if (ret < 0)
		goto out_shm;
#endif

	ret = flux_iokd_control_init();
	if (ret < 0)
#ifdef CONFIG_FLUX_FNET
		goto out_fnet;
#else
		goto out_shm;
#endif
	control_inited = true;

	ret = flux_iokd_bind_cpu(flux_iokd_cfg.cpu);
	if (ret < 0)
		goto out_ctl;

	ret = flux_iokd_kmod_init_sender();
	if (ret < 0)
		goto out_ctl;
	sender_inited = true;

	ret = flux_iokd_control_start();
	if (ret < 0)
		goto out_sender;
	FLUX_LOG(FLUX_LOG_INFO,
		 "ready cpu=%d sock=%s nic=%s mtu=%u tx_csum=%d net=%s\n",
		 flux_iokd_cfg.cpu, flux_iokd_cfg.sock_path,
		 flux_iokd_cfg.nic_pci_addr ? flux_iokd_cfg.nic_pci_addr :
					      "(none)",
		 flux_iokd_cfg.mtu, flux_iokd.tx_chksum_offload,
		 flux_iokd_cfg.no_network ? "none" :
		 flux_iokd.fnet_is_tap	  ? "tap" :
		 flux_iokd.fnet_has_port  ? "dpdk" :
					    "unknown");

	for (;;) {
		bool work_done = false;

		if (flux_iokd_shutdown_requested()) {
			ret = 0;
			break;
		}

#ifdef CONFIG_FLUX_FNET
		work_done |= flux_iokd_rx_burst();
		work_done |= flux_iokd_drain_completions();
		work_done |= flux_iokd_tx_burst();
		work_done |= flux_iokd_commands_rx();
#endif
		work_done |= flux_iokd_timers_run();

		if (!work_done)
			_mm_pause();
	}

	FLUX_LOG(FLUX_LOG_INFO, "shutdown start ret=%d\n", ret);
	flux_iokd_request_all_clients_exit(SIGTERM);

out_sender:
	if (sender_inited)
		flux_iokd_kmod_fini_sender();
out_ctl:
	if (control_inited)
		flux_iokd_control_fini();
#ifdef CONFIG_FLUX_FNET
	flux_iokd_fnet_stop();
#endif
	flux_iokd_retire_all_clients();
	flux_iokd_rx_process_pending_clients();
	flux_iokd_reap_retired();
#ifdef CONFIG_FLUX_FNET
out_fnet:
	flux_iokd_fnet_fini();
#endif
out_shm:
	flux_iokd_cpu_allocator_fini();
	if (shm_mapped)
		flux_iokd_kmod_unmap_shm();
out_cfg:
	FLUX_LOG(FLUX_LOG_INFO, "exit ret=%d\n", ret);
	flux_iokd_config_cleanup();
	return ret;
}
