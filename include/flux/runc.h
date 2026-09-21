#ifndef _FLUX_RUNC_H
#define _FLUX_RUNC_H

#include <stdbool.h>
#include <stdint.h>

#include <flux/base.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flux_oci_mount {
	char *destination;
	char *source;
	char *type;
	char *data;
	unsigned long flags;
	bool use_hostfs;
};

struct flux_oci_user {
	uid_t uid;
	gid_t gid;
	gid_t *additional_gids;
	int additional_gid_num;
	bool has_uid;
	bool has_gid;
};

struct flux_oci_capabilities {
	unsigned long long bounding;
	unsigned long long effective;
	unsigned long long permitted;
	unsigned long long inheritable;
	bool has_bounding;
	bool has_effective;
	bool has_permitted;
	bool has_inheritable;
};

struct flux_oci_rlimit {
	unsigned int resource;
	unsigned long soft;
	unsigned long hard;
	bool has_soft;
	bool has_hard;
};

enum flux_oci_device_access {
	FLUX_OCI_DEVICE_READ = 1U << 0,
	FLUX_OCI_DEVICE_WRITE = 1U << 1,
	FLUX_OCI_DEVICE_MKNOD = 1U << 2,
};

struct flux_oci_device_rule {
	long long major;
	long long minor;
	unsigned int access;
	char type;
	bool allow;
	bool has_allow;
	bool has_major;
	bool has_minor;
};

struct flux_oci_memory_resources {
	int64_t limit;
	int64_t reservation;
	int64_t swap;
	uint64_t swappiness;
	bool disable_oom_killer;
	bool use_hierarchy;
	bool check_before_update;
	bool has_limit;
	bool has_reservation;
	bool has_swap;
	bool has_swappiness;
	bool has_disable_oom_killer;
	bool has_use_hierarchy;
	bool has_check_before_update;
};

struct flux_oci_cpu_resources {
	uint64_t shares;
	int64_t quota;
	uint64_t period;
	uint64_t burst;
	int64_t realtime_runtime;
	uint64_t realtime_period;
	int64_t idle;
	char *cpus;
	char *mems;
	bool has_shares;
	bool has_quota;
	bool has_period;
	bool has_burst;
	bool has_realtime_runtime;
	bool has_realtime_period;
	bool has_idle;
	bool has_cpus;
	bool has_mems;
};

struct flux_oci_pids_resources {
	int64_t limit;
	bool has_limit;
};

struct flux_oci_block_io_throttle {
	int64_t major;
	int64_t minor;
	uint64_t rate;
};

struct flux_oci_block_io_weight_device {
	int64_t major;
	int64_t minor;
	uint16_t weight;
	uint16_t leaf_weight;
	bool has_weight;
	bool has_leaf_weight;
};

struct flux_oci_block_io_resources {
	uint16_t weight;
	uint16_t leaf_weight;
	bool has_weight;
	bool has_leaf_weight;
	struct flux_oci_block_io_weight_device *weight_devices;
	int weight_device_num;
	struct flux_oci_block_io_throttle *throttle_read_bps;
	int throttle_read_bps_num;
	struct flux_oci_block_io_throttle *throttle_write_bps;
	int throttle_write_bps_num;
	struct flux_oci_block_io_throttle *throttle_read_iops;
	int throttle_read_iops_num;
	struct flux_oci_block_io_throttle *throttle_write_iops;
	int throttle_write_iops_num;
};

struct flux_oci_network_priority {
	char *name;
	uint32_t priority;
};

struct flux_oci_network_resources {
	uint32_t class_id;
	bool has_class_id;
	struct flux_oci_network_priority *priorities;
	int priority_num;
};

struct flux_oci_resources {
	struct flux_oci_memory_resources memory;
	struct flux_oci_cpu_resources cpu;
	struct flux_oci_pids_resources pids;
	struct flux_oci_block_io_resources block_io;
	struct flux_oci_network_resources network;
	bool has_memory;
	bool has_cpu;
	bool has_pids;
	bool has_block_io;
	bool has_network;
};

struct flux_oci_sysctl {
	char *name;
	char *value;
};

enum flux_oci_namespace_flag {
	FLUX_OCI_NS_PID = 1U << 0,
	FLUX_OCI_NS_IPC = 1U << 1,
	FLUX_OCI_NS_UTS = 1U << 2,
	FLUX_OCI_NS_MOUNT = 1U << 3,
	FLUX_OCI_NS_NETWORK = 1U << 4,
	FLUX_OCI_NS_CGROUP = 1U << 5,
	FLUX_OCI_NS_TIME = 1U << 6,
};

struct flux_oci_cfg {
	char *oci_version;
	char *bundle_dir;
	char *config_path;
	char *rootfs_path;
	bool rootfs_readonly;
	char *cwd;
	char *hostname;
	char *exec_path;
	char **argv;
	int argc;
	char **env;
	int env_num;
	struct flux_oci_user user;
	struct flux_oci_capabilities capabilities;
	struct flux_oci_rlimit *rlimits;
	int rlimit_num;
	struct flux_oci_device_rule *device_rules;
	int device_rule_num;
	struct flux_oci_resources resources;
	struct flux_oci_sysctl *sysctls;
	int sysctl_num;
	bool no_new_privileges;
	struct flux_oci_mount *mounts;
	int mount_num;
	char **masked_paths;
	int masked_paths_num;
	char **readonly_paths;
	int readonly_paths_num;
	char *cgroups_path;
	unsigned int namespace_flags;
	unsigned int console_width;
	unsigned int console_height;
	bool has_console_size;
	bool terminal;
};

int flux_runc_set_bundle(const char *bundle_dir);
int flux_runc_set_bundle_config(const char *bundle_dir,
				const char *config_path);
int flux_runc_load_bundle(void);
int flux_runc_prepare_container_fs(void);
int flux_runc_enter_container_fs(void);
void flux_runc_unload(void);
bool flux_runc_bundle_enabled(void);
const struct flux_oci_cfg *flux_oci_cfg_get(void);
bool flux_oci_device_allowed(const struct flux_oci_cfg *oci, char type,
			     unsigned int major, unsigned int minor,
			     unsigned int access);

#ifdef __cplusplus
}
#endif

#endif
