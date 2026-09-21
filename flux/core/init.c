#define FLUX_FMT "flux: "

#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <time.h>
#include <ucontext.h>

#include <kernel/asm/flux_oci.h>
#include <flux.h>
#include <flux/mpk.h>
#include <flux/rewrite.h>
#include <flux/runc.h>

#define FLUX_RUNTIME_STACK_SIZE (8 * 1024 * 1024)
#include "oci.h"
#include "io/iok_client.h"
#include "host/uintr.h"
#ifdef CONFIG_FLUX_SPDK
#include "io/spdk.h"
#endif

struct flux_env flux_env = {
	.mem_size = 4 * GB,
	.dma_size = 256 * MB,
	.spdk_zero_copy = 0,
	.fnet_enabled = false,
	.malloc_hook_enabled = false,
};

static ucontext_t flux_exit_context[CONFIG_FLUX_MAX_CPUS];
static atomic_bool flux_exit_resume[CONFIG_FLUX_MAX_CPUS];

/*
 * Return from the Flux kernel stack to the saved host pthread frame without
 * invoking glibc's longjmp cleanup unwinder.  Flux can be interrupted
 * while libc has a cleanup record on its stack; unwinding that record with
 * the restored host TLS may run an unrelated cleanup handler and deadlock.
 */
void flux_thread_longjmp(int cpu)
{
	if (cpu < 0 || cpu >= CONFIG_FLUX_MAX_CPUS)
		abort();

	atomic_store_explicit(&flux_exit_resume[cpu], true,
			      memory_order_release);
	if (setcontext(&flux_exit_context[cpu]) < 0)
		abort();
	abort();
}

struct flux_pcpu_args {
	flux_thread_t th; /* host thread handle  */
	int cpu; /* logical cpu id */
	int host_cpu; /* physical cpu */
	volatile int ready;
	volatile int failed;
};
static struct flux_pcpu_args *pcpu_start_args;

struct flux_main_args {
	int argc;
	char **argv;
	const char *filename;
};
static struct flux_main_args main_start_args;

#define FLUX_EXIT_SYNC_TIMEOUT_NS (5ULL * 1000 * 1000 * 1000)
#define FLUX_PERCPU_EXIT_TIMEOUT_NS (20ULL * 1000 * 1000 * 1000)

static uint64_t flux_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

static int flux_exec_ring_map_current(void)
{
	const char *name = getenv(FLUX_EXEC_RING_ENV);
	struct flux_exec_ring *ring;
	size_t len;
	int fd;
	void *addr;

	if (!name || !name[0])
		return 0;

	len = align_up(sizeof(*ring), (size_t)CACHE_LINE_SIZE);
	fd = shm_open(name, O_RDWR, 0600);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "exec ring open %s failed: %s\n", name,
			 strerror(errno));
		return -1;
	}

	addr = mmap((void *)FLUX_EXEC_RING_MAP_ADDR, len,
		    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE,
		    fd, 0);
	close(fd);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "exec ring mmap %s failed: %s\n", name,
			 strerror(errno));
		return -1;
	}
	if (addr != (void *)FLUX_EXEC_RING_MAP_ADDR) {
		munmap(addr, len);
		FLUX_LOG(FLUX_LOG_ERR,
			 "exec ring mapped at unexpected address %p\n", addr);
		return -1;
	}

	unsetenv(FLUX_EXEC_RING_ENV);
	return 0;
}

struct flux_run_cfg *run_cfg;
char **flux_extra_envp;

struct flux_host_info flux_host;
static unsigned long flux_host_fsbases[CONFIG_FLUX_MAX_CPUS];

static atomic_int exit_sync_done = 0;
static atomic_int exit_sync_arrived = 0;

static void flux_save_host_fsbase(int cpu)
{
	asm volatile("rdfsbase %0" : "=r"(flux_host_fsbases[cpu]));
}

static void flux_restore_host_fsbase(int cpu)
{
	asm volatile("wrfsbase %0" : : "r"(flux_host_fsbases[cpu]));
}

int flux_run_cfg_load_current(void)
{
	char *path = getenv("FLUX_RUN_CFG_FILE");
	struct flux_run_cfg *cfg_args = flux_run_cfg_from_args();
	char *buf = NULL;
	int ret = -1;
	int fd = -1;
	int len;

	if (!path) {
		run_cfg = cfg_args;
		if (!run_cfg) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "no run cfg file and no run cfg args\n");
			return -1;
		}
		return 0;
	}

	run_cfg = calloc(1, sizeof(*run_cfg));
	if (!run_cfg) {
		perror("run cfg malloc");
		return -1;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "run cfg open %s\n", path);
		goto err;
	}

	len = lseek(fd, 0, SEEK_END);
	if (len < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "run cfg size check (lseek)\n");
		goto err;
	}
	if (len > 0) {
		lseek(fd, 0, SEEK_SET);

		buf = malloc(len + 1);
		if (!buf) {
			FLUX_LOG(FLUX_LOG_ERR, "run cfg buf malloc\n");
			goto err;
		}

		if (read(fd, buf, len) < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "run cfg read\n");
			goto err;
		}
		buf[len] = '\0';

		ret = flux_run_cfg_load_json(run_cfg, buf);
		if (ret < 0)
			goto err;
	} else {
		ret = 0;
	}

	if (cfg_args && flux_run_cfg_apply_overrides(run_cfg, cfg_args) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to apply command line run cfg overrides\n");
		goto err;
	}
out:
	free(buf);
	if (fd >= 0)
		close(fd);
	return ret;
err:
	free(run_cfg);
	run_cfg = NULL;
	ret = -1;
	goto out;
}

void flux_run_cfg_free_current(void)
{
	free(run_cfg);
}

extern struct flux_spdk_dev *spdk_dev;

static int do_mount_hostfs_from_cfg(const char *spec)
{
	const char *sep;
	size_t host_path_len;
	char host_path[FLUX_PATH_MAX];
	struct flux_mount_spec entry;
	int err;

	if (!spec)
		return -FLUX_EINVAL;

	sep = strchr(spec, ':');
	if (!sep || sep == spec || sep[1] == '\0') {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"invalid --hostfs_mounts format '%s', expected <host_path>:<flux_path>\n",
			spec);
		return -FLUX_EINVAL;
	}

	host_path_len = (size_t)(sep - spec);
	if (host_path_len >= sizeof(host_path))
		return -FLUX_ENAMETOOLONG;

	memcpy(host_path, spec, host_path_len);
	host_path[host_path_len] = '\0';

	if (sep[1] != '/') {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"invalid --hostfs_mounts target '%s', flux path must be absolute\n",
			sep + 1);
		return -FLUX_EINVAL;
	}

	entry = (struct flux_mount_spec){
		.source = host_path,
		.target = sep + 1,
		.type = "flux_hostfs",
		.data = NULL,
		.mode = 0755,
		.flags = flux_hostfs_mount_flags(),
	};

	err = flux_mount_with_mkdir(&entry);
	return err;
}

int flux_init_hostfs(void)
{
	char host_cwd[FLUX_PATH_MAX];
	char host_third_party[FLUX_PATH_MAX];
	int err;

	if (flux_runc_bundle_enabled())
		return 0;

	if (!getcwd(host_cwd, sizeof(host_cwd))) {
		err = -errno;
		FLUX_LOG(FLUX_LOG_DEBUG, "getcwd(): %s\n", flux_strerror(err));
		return err;
	}

	if (snprintf(host_third_party, sizeof(host_third_party),
		     "%s/third-party",
		     host_cwd) >= (int)sizeof(host_third_party))
		return -FLUX_ENAMETOOLONG;

	const struct flux_mount_spec hostfs_mounts[] = {
		{ host_third_party, "/third-party", "flux_hostfs", NULL, 0755,
		  flux_hostfs_mount_flags() },
	};

	for (size_t i = 0; i < sizeof(hostfs_mounts) / sizeof(hostfs_mounts[0]);
	     i++) {
		err = flux_mount_with_mkdir(&hostfs_mounts[i]);
		if (err < 0)
			return err;
	}

	if (run_cfg && run_cfg->hostfs_mounts) {
		for (int i = 0; i < run_cfg->hostfs_mounts_num; i++) {
			err = do_mount_hostfs_from_cfg(
				run_cfg->hostfs_mounts[i]);
			if (err < 0)
				return err;
		}
	}
	return 0;
}

int flux_init_rootfs(void)
{
	static const struct flux_mount_spec rootfs_mounts[] = {
		{ "devtmpfs", "/dev", "devtmpfs", NULL, 0755 },
		/*
		 * nodev is an MS_NODEV mount flag, not a tmpfs data parameter.
		 * The raw Flux mount syscall rejects it in the data string.
		 */
		{ "tmpfs", "/dev/shm", "tmpfs", "mode=1777", 0777,
		  FLUX_MS_NODEV },
		{ "tmpfs", "/mnt", "tmpfs", "mode=0777", 0700 },
		{ "tmpfs", "/tmp", "tmpfs", "mode=0777", 0777 },
		{ "sysfs", "/sys", "sysfs", NULL, 0700 },
		{ "tmpfs", "/run", "tmpfs", "mode=0700", 0700 },
		{ "proc", "/proc", "proc", NULL, 0755 },
	};
	int err;

	if (flux_runc_bundle_enabled()) {
		err = flux_runc_prepare_container_fs();
		if (err < 0)
			return err;

		return flux_runc_enter_container_fs();
	}

	flux_sys_chdir("/");

	for (size_t i = 0; i < sizeof(rootfs_mounts) / sizeof(rootfs_mounts[0]);
	     i++) {
		err = flux_mount_with_mkdir(&rootfs_mounts[i]);
		FLUX_LOG(FLUX_LOG_DEBUG, "mount(%s@%s): %s\n",
			 rootfs_mounts[i].source, rootfs_mounts[i].target,
			 flux_strerror(err));
		/*
		 * CONFIG_DEVTMPFS_MOUNT may have mounted /dev before Flux
		 * initializes the remaining root filesystem.  Reusing that
		 * exact mount is equivalent to completing this entry; keep
		 * EBUSY fatal for every other rootfs mount.
		 */
		if (err == -FLUX_EBUSY &&
		    !strcmp(rootfs_mounts[i].type, "devtmpfs") &&
		    !strcmp(rootfs_mounts[i].target, "/dev"))
			continue;
		if (err < 0)
			return err;
	}

	err = flux_sys_mknod("/dev/urandom", FLUX_S_IFCHR | 0666,
			     FLUX_MKDEV(1, 9));
	if (err < 0 && err != -FLUX_EEXIST) {
		FLUX_LOG(FLUX_LOG_DEBUG, "mknod(/dev/urandom): %s\n",
			 flux_strerror(err));
		return err;
	}

	return 0;
}

/*
 * Mirror the effective kernel bootmem layout so the host can reclaim the whole
 * kernel memory window after shutdown.
 */
static inline size_t flux_kernel_dma_size(void)
{
	size_t size = flux_env.dma_size;

	if (!size)
		return 0;
	if (size < 256 * MB)
		size = 256 * MB;

	return align_up(size, 256 * MB);
}

static inline size_t flux_kernel_mem_size(void)
{
	return align_up((size_t)flux_env.mem_size, PGSIZE_4KB) +
	       flux_kernel_dma_size();
}

static int flux_reclaim_kernel_pages(void)
{
	void *base = (void *)FLUX_MEMORY_ADDR;
	size_t len = flux_kernel_mem_size();
	void *addr;

	if (!len)
		return 0;

	if (munmap(base, len) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to unmap kernel memory range [%p, %p)\n", base,
			 base + len);
		return -1;
	}

#ifdef MAP_FIXED_NOREPLACE
	addr = mmap(base, len, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (addr == MAP_FAILED) {
		/* Fallback for older kernels that may not support NOREPLACE. */
		addr = mmap(base, len, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	}
#else
	addr = mmap(base, len, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
	if (addr != base) {
		if (addr != MAP_FAILED)
			munmap(addr, len);
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to quarantine kernel memory range [%p, %p)\n",
			 base, base + len);
		return -1;
	}

	FLUX_LOG(FLUX_LOG_INFO,
		 "kernel memory reclaimed and quarantined [%p, %p)\n", base,
		 base + len);

	return 0;
}

static void flux_percpu_exit_sync(int cpu)
{
	int nr = flux_env.nr_cpus;

	atomic_fetch_add(&exit_sync_arrived, 1);
	if (cpu == 0) {
		uint64_t deadline =
			flux_monotonic_ns() + FLUX_EXIT_SYNC_TIMEOUT_NS;

		while (atomic_load(&exit_sync_arrived) != nr &&
		       flux_monotonic_ns() < deadline)
			sched_yield();
		if (atomic_load(&exit_sync_arrived) != nr)
			FLUX_LOG(FLUX_LOG_WARN,
				 "exit sync timed out on cpu 0: arrived=%d/%d\n",
				 atomic_load(&exit_sync_arrived), nr);
		atomic_store(&exit_sync_done, 1);
	} else {
		/*
		 * A follower may leave Flux well before CPU0 begins shutdown.
		 * Its own arrival time therefore cannot define the barrier deadline:
		 * doing so lets it tear down host state before the coordinator has
		 * observed the other CPUs.  CPU0 has the bounded wait above and always
		 * publishes the decision; the isolated runtime remains protected by the
		 * runner's process deadline if CPU0 itself never returns.
		 */
		while (!atomic_load(&exit_sync_done))
			sched_yield();
	}
}

static void flux_main_entry(void *unused)
{
	int err;

	FLUX_LOG(FLUX_LOG_INFO, "kernel main entry\n");

	/* init kernel VFS */
	if (flux_init_rootfs() < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init rootfs\n");
		goto out;
	}

	/* init host FS */
	if (flux_init_hostfs() < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init hostfs\n");
		goto out;
	}

	/* setup kernel sysctl parameters */
	flux_sysctl("vm.dirty_ratio", "100");
	flux_sysctl("vm.dirty_background_ratio", "100");
	flux_sysctl("vm.swappiness", "0");

	flux_run_cfg_apply_post(run_cfg);

#ifdef CONFIG_FLUX_FNET
	/* register fnet net device */
	if (flux_fnet_register_dev() < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to register fnet device\n");
		goto out;
	}
#endif

#ifdef CONFIG_FLUX_SPDK
	int ret;

	/* register and mount SPDK block device */
	if ((ret = flux_spdk_register_dev(spdk_dev)) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to register spdk device\n");
		goto out;
	}
#endif

	/* The runtime arena belongs in every fork and fresh exec projection. */
	if (flux_malloc_hooks_init() < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init shared malloc arena\n");
		goto out;
	}
	flux_malloc_hooks_enable();

	if (flux_apply_oci_process_state() < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to apply OCI process settings\n");
		goto out;
	}

	if (flux_kmod_disable_mmap_hooks() < 0)
		goto out;
#ifdef CONFIG_FLUX_MPK
	if (flux_mpk_uaccess_init() < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to initialize MPK uaccess policy\n");
		goto out;
	}
#endif

	char **exec_envp = NULL;

	exec_envp = flux_build_envp();
	if (!exec_envp)
		goto out;
	err = flux_kernel_exec(main_start_args.filename, main_start_args.argv,
			       exec_envp);
	flux_free_envp(exec_envp);
	if (err < 0)
		FLUX_LOG(FLUX_LOG_ERR, "failed to execute application: %s (%d)\n",
			 flux_strerror(err), err);

	flux_sys_sync();
out:
	flux_sys_halt();
}

static atomic_int bind_done = 0;
static atomic_int bind_failed = 0;

static __always_inline void flux_uintr_enable_once(void)
{
	asm volatile("stui" : : : "memory");
}

static int flux_init_boot_cmdline(void)
{
	flux_env.boot_cmdline = malloc(FLUX_BOOT_CMDLINE_SIZE);
	if (!flux_env.boot_cmdline)
		return -FLUX_ENOMEM;

	snprintf(flux_env.boot_cmdline, FLUX_BOOT_CMDLINE_SIZE,
		 "%s mem=%s dma_size=%s", run_cfg->boot_cmdline ?: "",
		 run_cfg->mem_size ?: "4G", run_cfg->dma_size ?: "256M");

	return 0;
}

static int flux_disable_host_rseq(void)
{
#ifdef RSEQ_SIG
	struct rseq *rseq;

	/*
	 * Synthetic host-mm switches drop the worker's scheduler MM-CID.
	 * Unregister host glibc's rseq before the first switch, with or without
	 * MPK; a later host return must not publish an invalid MM-CID into TLS.
	 * MPK also prevents host rseq writes to protected runtime TLS. Flux
	 * application rseq remains independently registered and managed by Flux.
	 */
	if (!__rseq_size)
		return 0;

	rseq = (void *)((char *)__builtin_thread_pointer() + __rseq_offset);
	if (syscall(SYS_rseq, rseq, __rseq_size, RSEQ_FLAG_UNREGISTER,
		    RSEQ_SIG) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to unregister host rseq: %s\n",
			 strerror(errno));
		return -1;
	}
	rseq->cpu_id = RSEQ_CPU_ID_REGISTRATION_FAILED;
#endif
	return 0;
}

static void flux_percpu_entry(void *arg)
{
	struct flux_pcpu_args *args = arg;
	int cpu = args->cpu;
	int host_cpu = args->host_cpu;
	bool fsbase_saved = false;
	volatile bool failed = false;
	bool kernel_exited = false;

	FLUX_LOG(FLUX_LOG_INFO, "cpu %d (host cpu %d) entry (tid %ld)\n", cpu,
		 host_cpu, syscall(__flux__NR_gettid));

	/* set scheduling affinity */
	if (flux_bind_single_cpu(host_cpu) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to bind cpu %d\n", host_cpu);
		failed = true;
		goto cleanup;
	}

	if (flux_disable_host_rseq() < 0) {
		failed = true;
		goto cleanup;
	}
#ifdef CONFIG_FLUX_MPK
	flux_mpk_enter_kernel();
#endif

	if (flux_signal_init_percpu() < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to prepare signals on percpu thread\n");
		failed = true;
		goto cleanup;
	}

	flux_save_host_fsbase(cpu);
	fsbase_saved = true;

	if (flux_kmod_init_percpu(flux_uintr_handler,
				  flux_uintr_signal_stack, cpu,
				  flux_host_fsbases[cpu],
				  flux_signal_stack_base(),
				  flux_signal_stack_slot_size()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init kmod percpu\n");
		failed = true;
		goto cleanup;
	}

	if (flux_signal_enable_percpu() < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to enable signals on percpu thread\n");
		failed = true;
		goto cleanup;
	}

	atomic_fetch_add(&bind_done, 1);
	while (atomic_load(&bind_done) != flux_env.nr_cpus) {
		if (atomic_load(&bind_failed)) {
			failed = true;
			goto cleanup;
		}
		sched_yield();
	}

	__atomic_store_n(&args->ready, 1, __ATOMIC_RELEASE);

	if (getcontext(&flux_exit_context[cpu]) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to save cpu %d host context: %s\n", cpu,
			 strerror(errno));
		failed = true;
		goto cleanup;
	}
	if (!atomic_exchange_explicit(&flux_exit_resume[cpu], false,
				      memory_order_acquire)) {
		/*
		 * kmod installed this CPU's initial UINTR xstate with UIF clear.
		 * Enable delivery exactly once, after all host-side setup and saved
		 * exit context work, immediately before the non-returning kernel entry.
		 */
		flux_uintr_enable_once();

		/* kernel entry */
		if (!cpu) {
			flux_host.ops = &flux_host_ops;
			flux_host.elf_interpreter = run_cfg->ld_path;
			flux_host.hwcap = getauxval(AT_HWCAP);
			flux_host.hwcap2 = getauxval(AT_HWCAP2);
#ifdef CONFIG_FLUX_SPDK
			flux_host.spdk = &flux_spdk;
#endif
			flux_host.uipi = flux_uipi;
			flux_host.max_cpus = flux_env.max_cpus;
			flux_host.nr_cpus = flux_env.nr_cpus;
			flux_host.main = flux_main_entry;
			flux_host.fsbases = flux_host_fsbases;
			flux_start_kernel(&flux_host, flux_env.boot_cmdline);
		} else {
#ifdef CONFIG_FLUX_SMP
			flux_start_kernel_secondary(cpu);
#endif
		}
	}
	kernel_exited = true;

	FLUX_LOG(FLUX_LOG_INFO, "cpu %d exiting\n", cpu);

cleanup:
	if (kernel_exited)
		flux_percpu_exit_sync(cpu);

	if (fsbase_saved)
		flux_restore_host_fsbase(cpu);

	if (flux_signal_exit_percpu() < 0)
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to teardown percpu signal state on exit\n");

	if (flux_kmod_exit_percpu() < 0)
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to teardown kmod percpu state on exit\n");

	if (failed) {
		args->failed = 1;
		atomic_store(&bind_failed, 1);
	}

	__atomic_store_n(&args->ready, 2, __ATOMIC_RELEASE);
	/*
	 * Both pthread_exit() and a normal pthread start-routine return eventually
	 * enter glibc's common thread-exit machinery. Under fork-heavy workloads
	 * that path can stall even though every Flux teardown stage above has
	 * completed. These dedicated percpu threads have no TLS
	 * destructors or pthread cleanup handlers to run, so terminate only this
	 * thread through the kernel.  CLONE_CHILD_CLEARTID still wakes
	 * pthread_join(), allowing the parent to reclaim the pthread descriptor.
	 */
	syscall(__flux__NR_exit, 0);
	__builtin_unreachable();
}

static void flux_wait_percpu_exit(struct flux_pcpu_args *args)
{
	/*
	 * This wait spans the complete Flux lifetime, not just teardown.  Keep it
	 * unbounded here so normal tests lasting more than a few seconds are not
	 * mistaken for cleanup failures.  The per-test runner owns the real
	 * runtime deadline and can terminate this isolated process safely.
	 */
	while (__atomic_load_n(&args->ready, __ATOMIC_ACQUIRE) != 2)
		sched_yield();
}

static bool flux_wait_percpu_exit_until(struct flux_pcpu_args *args,
					uint64_t deadline)
{
	while (__atomic_load_n(&args->ready, __ATOMIC_ACQUIRE) != 2) {
		if (flux_monotonic_ns() >= deadline)
			return false;
		sched_yield();
	}

	return true;
}

static __attribute__((noreturn)) void
flux_abort_percpu_cleanup(int cpu)
{
	FLUX_LOG(FLUX_LOG_ERR,
		 "timed out waiting for cpu %d host thread to exit; terminating runtime\n",
		 cpu);
	/*
	 * A live per-CPU thread can still hold Flux locks or reference the Flux
	 * mappings.  Do not run normal process teardown or free shared state from
	 * underneath it.  This is the isolated Flux child, so terminating the
	 * complete process is the only safe cleanup path.
	 */
	_Exit(EXIT_FAILURE);
}

static int flux_bind_cpus(void)
{
	int i;
	int ret = 0;
	bool init_lifetime_done = false;
	uint64_t cleanup_deadline = 0;

	atomic_store(&bind_done, 0);
	atomic_store(&bind_failed, 0);

	pcpu_start_args =
		calloc(flux_env.nr_cpus, sizeof(struct flux_pcpu_args));
	if (!pcpu_start_args) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to alloc percpu_start_args\n");
		return -1;
	}

	for (i = 0; i < flux_env.nr_cpus; i++) {
		memset(&pcpu_start_args[i], 0, sizeof(struct flux_pcpu_args));
		pcpu_start_args[i].cpu = i;
		pcpu_start_args[i].host_cpu = flux_env.cpu_list[i];
		pcpu_start_args[i].th = flux_host_ops.thread_create(
			flux_percpu_entry, (void *)&pcpu_start_args[i],
			"percpu_entry");
		if (!pcpu_start_args[i].th) {
			atomic_store(&bind_failed, 1);
			ret = -1;
			goto out;
		}
	}

out:
	for (i = 0; i < flux_env.nr_cpus; i++) {
		if (!pcpu_start_args[i].th)
			continue;

		if (!init_lifetime_done) {
			/*
			 * CPU0 owns the Flux init lifetime.  Wait for it without a local
			 * deadline so a legitimate long-running test is not mistaken for
			 * teardown.  Only after it exits are remaining CPUs in cleanup.
			 */
			flux_wait_percpu_exit(&pcpu_start_args[i]);
			init_lifetime_done = true;
			cleanup_deadline = flux_monotonic_ns() +
					   FLUX_PERCPU_EXIT_TIMEOUT_NS;
		} else if (!flux_wait_percpu_exit_until(&pcpu_start_args[i],
							 cleanup_deadline)) {
			flux_abort_percpu_cleanup(pcpu_start_args[i].cpu);
		}

		if (flux_host_ops.thread_join(pcpu_start_args[i].th) < 0)
			flux_abort_percpu_cleanup(pcpu_start_args[i].cpu);
		if (pcpu_start_args[i].failed)
			ret = -1;
	}
	free(pcpu_start_args);

	if (flux_signal_restore_defaults() < 0)
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to restore default signal handlers\n");

	if (flux_env.malloc_hook_enabled)
		flux_malloc_hooks_disable();

	FLUX_LOG(FLUX_LOG_INFO, "all cpus exits\n");

	return ret;
}

/*
 * Switch to a bigger stack for Flux Env.
 */
int flux_stack_init(int (*entry)(int, char **), int argc, char **argv)
{
	void *stack;
	void *stack_top;
	int ret = 0;

	stack = mmap(NULL, FLUX_RUNTIME_STACK_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "mmap stack failed\n");
		return -FLUX_ENOMEM;
	}

	stack_top = (void *)((uintptr_t)stack + FLUX_RUNTIME_STACK_SIZE);
	stack_top = (void *)((uintptr_t)stack_top & ~0xFULL);

	FLUX_LOG(FLUX_LOG_INFO, "switch to bigger stack %p\n", stack_top);

#if defined(__x86_64__)
	__asm__ __volatile__("mov %%rsp, %%r12\n\t"
			     "mov %[stack_top], %%rsp\n\t"
			     "call *%[entry]\n\t"
			     "mov %%eax, %[ret]\n\t"
			     "mov %%r12, %%rsp\n\t"
			     : [ret] "=r"(ret)
			     : [stack_top] "r"(stack_top), [entry] "r"(entry),
			       "D"(argc), "S"(argv)
			     : "memory", "rax", "rcx", "rdx", "r8", "r9", "r10",
			       "r11", "r12");
#else
#error "only x86_64 supported"
#endif

	munmap(stack, FLUX_RUNTIME_STACK_SIZE);
	return ret;
}

static int flux_rodata_init(void)
{
	void *addr;
	struct flux_rodata *rodata;

	addr = mmap((void *)FLUX_RODATA_ADDR, sizeof(struct flux_rodata),
		    PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "mmap rodata failed\n");
		return -1;
	}
	memset(addr, 0, sizeof(struct flux_rodata));

	rodata = (struct flux_rodata *)addr;
	rodata->syscall_fast = flux_syscall_fast;
	rodata->syscall = flux_syscall;
	rodata->syscall_rewrite = flux_syscall_dispatch;
	rodata->syscall_sigreturn_nostack = flux_syscall_sigreturn_nostack;

	addr = mmap((void *)FLUX_MPK_RETURN_ADDR, FLUX_MPK_RETURN_AREA_SIZE,
		    PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "mmap return slots failed\n");
		return -1;
	}
	memset(addr, 0, FLUX_MPK_RETURN_AREA_SIZE);

	return 0;
}

static int flux_env_free(void)
{
	if (flux_env.cpu_list)
		free(flux_env.cpu_list);
	if (flux_env.boot_cmdline)
		free(flux_env.boot_cmdline);
	return 0;
}

int flux_env_init(int argc, char **argv)
{
	int ret;
	bool iok_client_inited = false;
	bool uintr_inited = false;

	FLUX_LOG(FLUX_LOG_INFO, "starting libos\n");

	main_start_args.argc = argc;
	main_start_args.argv = argv;
	main_start_args.filename = argv[0];

	if (flux_launch_get() && flux_launch_get()->filename)
		main_start_args.filename = flux_launch_get()->filename;


	if ((ret = flux_rodata_init()) < 0)
		goto out;

	if ((ret = flux_run_cfg_load_current()) < 0)
		goto out;

	if ((ret = flux_run_cfg_apply_pre(run_cfg)) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to apply run cfg\n");
		goto out_free_cfg;
	}

	if ((ret = flux_signal_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init signal handlers\n");
		goto out_free_env;
	}

	if ((ret = flux_init_boot_cmdline()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init boot cmdline\n");
		goto out_free_env;
	}

	if ((ret = flux_iok_client_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to connect flux_iokd: %d\n",
			 ret);
		goto out_free_env;
	}
	iok_client_inited = true;

	if ((ret = flux_init_cpus()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init cpus\n");
		goto out_free_env;
	}

	if ((ret = flux_kmod_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init flux kmod\n");
		goto out_free_env;
	}

	if ((ret = flux_exec_ring_map_current()) < 0)
		goto out_free_env;

	if ((ret = flux_uintr_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init uintr\n");
		goto out_free_env;
	}
	uintr_inited = true;

#ifdef CONFIG_FLUX_SPDK
	if ((ret = flux_spdk_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to init spdk\n");
		goto out_free_env;
	}
#endif

	if ((ret = flux_mpk_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to initialize MPK isolation\n");
		goto out_free_env;
	}
	if ((ret = flux_rewrite_init()) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to initialize executable rewriting\n");
		goto out_free_env;
	}

	if ((ret = flux_bind_cpus()) < 0)
		FLUX_LOG(FLUX_LOG_ERR, "failed to bind one or more CPUs\n");

	flux_iok_client_fini();
	iok_client_inited = false;
	if (flux_reclaim_kernel_pages() < 0)
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to reclaim/quarantine kernel pages\n");

	if (uintr_inited)
		flux_uintr_fini();

#ifdef CONFIG_FLUX_SPDK
	flux_spdk_fini();
#endif

	return ret;
out_free_env:
	if (iok_client_inited)
		flux_iok_client_fini();
	if (uintr_inited)
		flux_uintr_fini();
	flux_env_free();
out_free_cfg:
	flux_run_cfg_free_current();
out:
	return ret;
}
