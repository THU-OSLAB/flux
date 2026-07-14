#ifndef _FLUX_RUNC_H
#define _FLUX_RUNC_H

#include <stdbool.h>

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

struct flux_oci_cfg {
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
	bool no_new_privileges;
	struct flux_oci_mount *mounts;
	int mount_num;
	char **masked_paths;
	int masked_paths_num;
	char **readonly_paths;
	int readonly_paths_num;
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

#ifdef __cplusplus
}
#endif

#endif
