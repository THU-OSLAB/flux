#ifndef _FLUX_RUNTIME_H
#define _FLUX_RUNTIME_H

#include <flux/base.h>
#include <kernel/asm/flux_oci.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUX_RUN_CFG_JSON_TOKEN_MAX 300
#define FLUX_BOOT_CMDLINE_SIZE 4096

#define FLUX_SIGNAL_BRIDGE_ENV "FLUX_SIGNAL_BRIDGE_SIGNALS"
#define FLUX_DEFAULT_SIGNAL_BRIDGE FLUX_SIGNAL_CTRL_DOORBELL_STR
#define FLUX_EXEC_RING_ENV "FLUX_EXEC_RING_SHM"

/*
 * Runtime inputs loaded from CLI arguments and JSON.
 *
 * Most members stay as strings until startup resolves overrides and validates
 * them in one place.
 */
struct flux_run_cfg {
	/* Generic startup options. */
	char *debug;
	char *mount;
	char *boot_cmdline;
	char *sysctl;
	char *dump;
	char *mem_size;
	char *dma_size;
	char *nr_cpus;
	char *cpu_set;
	char *cpu_shares;
	char *cpu_quota;
	char *cpu_period;
	char *pid_limit;
	char *blkio_weight;
	char *network_class_id;
	char *network_priority;

	/* SPDK configuration. */
	char **spdk_bdf;
	int spdk_dev_num;
	char *spdk_zero_copy;
	char *spdk_fs_type;
	char *sync_on_shutdown;

	/* Host filesystem and signal policy. */
	char **hostfs_mounts;
	int hostfs_mounts_num;
	char *hostfs_writable;
	char **signals;
	int signals_num;

	/* Process environment inside the runtime. */
	char **env;
	int env_num;
	char *ld_path;

	/* External dataplane configuration. */
	char *nic_ip_addr;
	char *nic_ip_gw;
	char *nic_ip_mask;
	char *iok_sock_path;
};

/*
 * Process-wide runtime state resolved before Flux startup and then treated as
 * read-mostly by the runtime.
 */
struct flux_env {
	int debug;
	int dev_fd;
	char *boot_cmdline;
	unsigned long mem_size;
	unsigned long dma_size;

	/* Flux CPU placement on the host. */
	int nr_cpus;
	int max_cpus;
	int *cpu_list;

	/*
	 * Host CPU reserved for local control-path work such as timer fallback
	 * when an external owner is not used.
	 */
	int ctrl_cpu;

	/* Storage and dataplane feature toggles derived from the run cfg. */
	int spdk_zero_copy;
	int fnet_enabled;
	int malloc_hook_enabled;
};

extern struct flux_run_cfg *run_cfg;
extern struct flux_env flux_env;
extern struct flux_host_info flux_host;
extern struct flux_host_operations flux_host_ops;
extern struct flux_spdk flux_spdk;
extern char **flux_extra_envp;

/* Run cfg loading and override handling. */
int flux_run_cfg_parse_args(int argc, char **argv);
struct flux_run_cfg *flux_run_cfg_from_args(void);
int flux_run_cfg_apply_overrides(struct flux_run_cfg *base,
				 const struct flux_run_cfg *overrides);
int flux_run_cfg_load_json(struct flux_run_cfg *cfg, const char *jstr);
int flux_run_cfg_save_json(const struct flux_run_cfg *cfg, const char *path);
void flux_run_cfg_show(struct flux_run_cfg *cfg);
void flux_run_cfg_fini(struct flux_run_cfg *cfg);
int flux_run_cfg_apply_pre(struct flux_run_cfg *cfg);
int flux_run_cfg_apply_post(struct flux_run_cfg *cfg);
int flux_run_cfg_load_current(void);
void flux_run_cfg_free_current(void);

/* Flux startup and program loading. */
int flux_env_set_cpus(const int *cpu_list, int nr_cpus);
int flux_env_init(int argc, char **argv);
int flux_stack_init(int (*entry)(int, char **), int argc, char **argv);
int flux_init_cpus(void);
int flux_bind_single_cpu(int cpu);
char **flux_build_envp(void);
void flux_free_envp(char **envp);
int flux_fnet_register_dev(void);
void flux_thread_longjmp(int cpu);

/* Signal handling used by the host runtime. */
int flux_signal_init(void);
int flux_signal_init_percpu(void);
void *flux_signal_stack_base(void);
size_t flux_signal_stack_slot_size(void);
int flux_signal_enable_percpu(void);
int flux_signal_block_current(void);
int flux_signal_exit_percpu(void);
int flux_signal_restore_defaults(void);

/* Kernel-module and allocator hooks. */
int flux_kmod_init(void);
int flux_kmod_init_percpu(void *handler, void *synthetic_handler,
			  int logical_cpu,
			  unsigned long host_fsbase, void *signal_stack,
			  size_t signal_stack_slot_size);
int flux_kmod_exit_percpu(void);
int flux_kmod_disable_mmap_hooks(void);
int flux_malloc_hooks_init(void);
void flux_malloc_hooks_enable(void);
void flux_malloc_hooks_disable(void);

/*
 * Register a signal handler that loads the debug library on Ctrl-Z.
 *
 * Shell wrappers should ignore SIGTSTP themselves if they want Ctrl-Z to
 * reach Flux directly.
 */
void flux_register_dbg_handler(void);

int flux_set_fd_limit(unsigned int fd_limit);
int flux_init_rootfs(void);
int flux_init_hostfs(void);

/*
 * Allocate and free DMA-capable memory managed by the SPDK integration.
 *
 * Returns NULL on allocation failure.
 */
void *flux_spdk_dma_malloc(void *hint, size_t size, size_t align, int flags);
void flux_spdk_dma_free(void *addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif
