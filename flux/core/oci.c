#define FLUX_FMT "flux: "

#include <string.h>
#include <stdio.h>
#include <sys/prctl.h>

#include <kernel/linux/capability.h>
#include <kernel/linux/resource.h>
#include <flux.h>
#include <flux/runc.h>

#include "oci.h"

#ifndef CONFIG_FLUX_RUNC

int flux_apply_oci_process_state(void)
{
	return 0;
}

#else

#define FLUX_OCI_CGROUP_ROOT "/sys/fs/cgroup"
#define FLUX_OCI_CGROUP_LEAF "/sys/fs/cgroup/flux-oci"

static int flux_oci_resource_write(const char *path, const char *value)
{
	int fd;
	int ret;

	fd = flux_sys_open(path, FLUX_O_WRONLY, 0);
	if (fd < 0)
		return fd;
	ret = flux_sys_write(fd, value, strlen(value));
	if (ret >= 0 && (size_t)ret != strlen(value))
		ret = -FLUX_EIO;
	else if (ret >= 0)
		ret = 0;
	if (flux_sys_close(fd) < 0 && ret == 0)
		ret = -FLUX_EIO;
	return ret;
}

static int flux_oci_resource_write_i64(const char *name, int64_t value,
				       bool max_for_negative)
{
	char path[256];
	char buf[64];

	if (snprintf(path, sizeof(path), "%s/%s", FLUX_OCI_CGROUP_LEAF,
		     name) >= (int)sizeof(path))
		return -FLUX_ENAMETOOLONG;
	if (value < 0 && max_for_negative)
		snprintf(buf, sizeof(buf), "max\n");
	else
		snprintf(buf, sizeof(buf), "%lld\n", (long long)value);
	return flux_oci_resource_write(path, buf);
}

static int flux_oci_resource_write_u64(const char *name, uint64_t value)
{
	char path[256];
	char buf[64];

	if (snprintf(path, sizeof(path), "%s/%s", FLUX_OCI_CGROUP_LEAF,
		     name) >= (int)sizeof(path))
		return -FLUX_ENAMETOOLONG;
	snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)value);
	return flux_oci_resource_write(path, buf);
}

static uint64_t flux_oci_cpu_weight(uint64_t shares)
{
	if (shares == 0)
		return 100;
	if (shares < 2)
		shares = 2;
	if (shares > 262144)
		shares = 262144;
	return 1 + ((shares - 2) * 9999) / 262142;
}

static int flux_oci_enable_controller(const char *name)
{
	char value[64];

	if (snprintf(value, sizeof(value), "+%s\n", name) >=
	    (int)sizeof(value))
		return -FLUX_EOVERFLOW;
	return flux_oci_resource_write(
		FLUX_OCI_CGROUP_ROOT "/cgroup.subtree_control", value);
}

static int flux_oci_apply_internal_memory(
	const struct flux_oci_memory_resources *memory)
{
	int ret;

	if (memory->has_limit) {
		ret = flux_oci_resource_write_i64("memory.max", memory->limit,
						  true);
		if (ret < 0)
			return ret;
	}
	if (memory->has_reservation) {
		ret = flux_oci_resource_write_i64(
			"memory.low",
			memory->reservation < 0 ? 0 : memory->reservation, false);
		if (ret < 0)
			return ret;
	}
	if (memory->has_swap) {
		int64_t swap = memory->swap;

		if (swap >= 0 && memory->has_limit && memory->limit >= 0) {
			if (swap < memory->limit)
				return -FLUX_EINVAL;
			swap -= memory->limit;
		} else if (swap >= 0 && !memory->has_limit) {
			return -FLUX_EINVAL;
		}
		ret = flux_oci_resource_write_i64("memory.swap.max", swap, true);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int flux_oci_apply_internal_cpu(
	const struct flux_oci_cpu_resources *cpu)
{
	char value[128];
	uint64_t period;
	int ret;

	if (cpu->has_shares) {
		ret = flux_oci_resource_write_u64(
			"cpu.weight", flux_oci_cpu_weight(cpu->shares));
		if (ret < 0)
			return ret;
	}
	if (cpu->has_quota || cpu->has_period) {
		period = cpu->has_period && cpu->period ? cpu->period : 100000;
		if (cpu->has_quota && cpu->quota >= 0)
			snprintf(value, sizeof(value), "%lld %llu\n",
				 (long long)cpu->quota,
				 (unsigned long long)period);
		else
			snprintf(value, sizeof(value), "max %llu\n",
				 (unsigned long long)period);
		ret = flux_oci_resource_write(FLUX_OCI_CGROUP_LEAF "/cpu.max",
					      value);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_burst) {
		ret = flux_oci_resource_write_u64("cpu.max.burst", cpu->burst);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_idle) {
		ret = flux_oci_resource_write_i64("cpu.idle", cpu->idle, false);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int flux_apply_oci_internal_resources(const struct flux_oci_cfg *oci)
{
	const struct flux_oci_resources *resources;
	unsigned long flags = FLUX_MS_NOSUID | FLUX_MS_NODEV | FLUX_MS_NOEXEC;
	int ret;

	if (!oci)
		return 0;
	resources = &oci->resources;
	ret = flux_sys_mkdir("/sys", 0755);
	if (ret < 0 && ret != -FLUX_EEXIST)
		return ret;
	ret = flux_sys_mkdir("/sys/fs", 0755);
	if (ret < 0 && ret != -FLUX_EEXIST)
		return ret;
	ret = flux_sys_mkdir(FLUX_OCI_CGROUP_ROOT, 0755);
	if (ret < 0 && ret != -FLUX_EEXIST)
		return ret;

	ret = flux_sys_mount("none", FLUX_OCI_CGROUP_ROOT, "cgroup2", flags,
			     NULL);
	if (ret < 0)
		return ret;
	ret = flux_oci_enable_controller("memory");
	if (ret < 0)
		return ret;
	ret = flux_oci_enable_controller("cpu");
	if (ret < 0)
		return ret;
	ret = flux_oci_enable_controller("pids");
	if (ret < 0)
		return ret;
	ret = flux_sys_mkdir(FLUX_OCI_CGROUP_LEAF, 0755);
	if (ret < 0 && ret != -FLUX_EEXIST)
		return ret;

	/* Move init first so pids.max=0 remains a valid no-new-task limit. */
	ret = flux_oci_resource_write(FLUX_OCI_CGROUP_LEAF "/cgroup.procs",
				      "0\n");
	if (ret < 0)
		return ret;
	if (resources->has_memory) {
		ret = flux_oci_apply_internal_memory(&resources->memory);
		if (ret < 0)
			return ret;
	}
	if (resources->has_cpu) {
		ret = flux_oci_apply_internal_cpu(&resources->cpu);
		if (ret < 0)
			return ret;
	}
	if (resources->has_pids && resources->pids.has_limit) {
		ret = flux_oci_resource_write_i64(
			"pids.max", resources->pids.limit, true);
		if (ret < 0)
			return ret;
	}

	/* The host cgroup remains the non-bypassable outer hard limit. */
	return 0;
}

static int flux_apply_oci_capabilities(const struct flux_oci_cfg *oci)
{
	struct __flux__user_cap_data_struct data[2];
	flux_cap_user_header_t header;
	unsigned int cap;
	int ret;

	if (!oci || (!oci->capabilities.has_bounding &&
		     !oci->capabilities.has_effective &&
		     !oci->capabilities.has_permitted &&
		     !oci->capabilities.has_inheritable))
		return 0;

	header = (flux_cap_user_header_t) &
		 (struct __flux__user_cap_header_struct){
			 .version = _FLUX_LINUX_CAPABILITY_VERSION_3,
			 .pid = 0,
		 };

	ret = flux_sys_capget(header, data);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "OCI capget failed: %s\n",
			 flux_strerror(-ret));
		return ret;
	}

	if (oci->capabilities.has_bounding) {
		for (cap = 0; cap <= FLUX_CAP_LAST_CAP; cap++) {
			unsigned long long bit = 1ULL << cap;

			if (oci->capabilities.bounding & bit)
				continue;

			ret = flux_sys_prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);
			if (ret < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI PR_CAPBSET_DROP(%u) failed: %s\n",
					 cap, flux_strerror(-ret));
				return ret;
			}
		}
	}

	if (oci->capabilities.has_effective) {
		data[0].effective = (unsigned int)(oci->capabilities.effective &
						   0xffffffffULL);
		data[1].effective =
			(unsigned int)((oci->capabilities.effective >> 32) &
				       0xffffffffULL);
	}
	if (oci->capabilities.has_permitted) {
		data[0].permitted = (unsigned int)(oci->capabilities.permitted &
						   0xffffffffULL);
		data[1].permitted =
			(unsigned int)((oci->capabilities.permitted >> 32) &
				       0xffffffffULL);
	}
	if (oci->capabilities.has_inheritable) {
		data[0].inheritable =
			(unsigned int)(oci->capabilities.inheritable &
				       0xffffffffULL);
		data[1].inheritable =
			(unsigned int)((oci->capabilities.inheritable >> 32) &
				       0xffffffffULL);
	}

	ret = flux_sys_capset(header, data);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "OCI capset failed: %s\n",
			 flux_strerror(-ret));
		return ret;
	}

	return 0;
}

static int flux_apply_oci_user(const struct flux_oci_cfg *oci)
{
	int ret;

	if (!oci || (!oci->user.has_uid && !oci->user.has_gid))
		return 0;

	if (oci->user.has_gid || oci->user.additional_gid_num > 0) {
		ret = flux_sys_setgroups(oci->user.additional_gid_num,
					 oci->user.additional_gids);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "OCI setgroups(%d) failed: %s\n",
				 oci->user.additional_gid_num, flux_strerror(-ret));
			return ret;
		}
	}

	if (oci->user.has_gid) {
		ret = flux_sys_setresgid(oci->user.gid, oci->user.gid,
					 oci->user.gid);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "OCI setresgid(%u) failed: %s\n",
				 (unsigned int)oci->user.gid,
				 flux_strerror(-ret));
			return ret;
		}
	}

	if (oci->user.has_uid) {
		ret = flux_sys_setresuid(oci->user.uid, oci->user.uid,
					 oci->user.uid);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "OCI setresuid(%u) failed: %s\n",
				 (unsigned int)oci->user.uid,
				 flux_strerror(-ret));
			return ret;
		}
	}

	return 0;
}

static int flux_apply_oci_rlimits(const struct flux_oci_cfg *oci)
{
	int i;

	if (!oci || !oci->rlimits || oci->rlimit_num <= 0)
		return 0;

	for (i = 0; i < oci->rlimit_num; i++) {
		struct flux_rlimit rlim = {
			.rlim_cur = oci->rlimits[i].soft,
			.rlim_max = oci->rlimits[i].hard,
		};
		int ret;

		ret = flux_sys_setrlimit(oci->rlimits[i].resource, &rlim);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "OCI setrlimit(%u) failed: %s\n",
				 oci->rlimits[i].resource, flux_strerror(-ret));
			return ret;
		}
	}

	return 0;
}

static int flux_apply_oci_sysctls(const struct flux_oci_cfg *oci)
{
	int i;

	if (!oci)
		return 0;
	for (i = 0; i < oci->sysctl_num; i++) {
		int ret = flux_sysctl(oci->sysctls[i].name,
				      oci->sysctls[i].value);

		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "OCI sysctl %s=%s failed: %s\n",
				 oci->sysctls[i].name, oci->sysctls[i].value,
				 flux_strerror(-ret));
			return ret;
		}
	}
	return 0;
}

static bool flux_oci_has_capability_settings(const struct flux_oci_cfg *oci)
{
	if (!oci)
		return false;

	return oci->capabilities.has_bounding ||
	       oci->capabilities.has_effective ||
	       oci->capabilities.has_permitted ||
	       oci->capabilities.has_inheritable;
}

int flux_apply_oci_process_state(void)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	int err;
	bool keepcaps = false;

	if (!oci)
		return 0;

	FLUX_LOG(FLUX_LOG_INFO, "applying OCI process state\n");
	FLUX_LOG(FLUX_LOG_INFO, "applying OCI Flux resource controls\n");
	err = flux_apply_oci_internal_resources(oci);
	if (err < 0)
		return err;
	FLUX_LOG(FLUX_LOG_INFO, "applying OCI sysctls\n");
	err = flux_apply_oci_sysctls(oci);
	if (err < 0)
		return err;
	FLUX_LOG(FLUX_LOG_INFO, "applied OCI sysctls\n");

	if (oci->hostname) {
		FLUX_LOG(FLUX_LOG_INFO, "applying OCI hostname\n");
		err = flux_sys_sethostname(oci->hostname,
					   strlen(oci->hostname));
		if (err < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "OCI sethostname(%s) failed: %s\n",
				 oci->hostname, flux_strerror(-err));
			return err;
		}
	}

	if (oci->cwd) {
		FLUX_LOG(FLUX_LOG_INFO, "applying OCI cwd\n");
		err = flux_sys_chdir(oci->cwd);
		if (err < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "OCI chdir(%s) failed: %s\n",
				 oci->cwd, flux_strerror(-err));
			return err;
		}
	}

	if (oci->user.has_uid || oci->user.has_gid) {
		if (flux_oci_has_capability_settings(oci)) {
			err = flux_sys_prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
			if (err < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "OCI PR_SET_KEEPCAPS(1) failed: %s\n",
					 flux_strerror(-err));
				return err;
			}
			keepcaps = true;
		}
	}

	FLUX_LOG(FLUX_LOG_INFO, "applying OCI user\n");
	err = flux_apply_oci_user(oci);
	if (err < 0)
		return err;

	FLUX_LOG(FLUX_LOG_INFO, "applying OCI capabilities\n");
	err = flux_apply_oci_capabilities(oci);
	if (err < 0)
		return err;

	if (keepcaps) {
		err = flux_sys_prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
		if (err < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "OCI PR_SET_KEEPCAPS(0) failed: %s\n",
				 flux_strerror(-err));
			return err;
		}
	}

	FLUX_LOG(FLUX_LOG_INFO, "applying OCI rlimits\n");
	err = flux_apply_oci_rlimits(oci);
	if (err < 0)
		return err;

	if (oci->no_new_privileges) {
		FLUX_LOG(FLUX_LOG_INFO, "applying OCI no-new-privileges\n");
		err = flux_sys_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		if (err < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "OCI PR_SET_NO_NEW_PRIVS failed: %s\n",
				 flux_strerror(-err));
			return err;
		}
	}

	FLUX_LOG(FLUX_LOG_INFO, "applied OCI process state\n");
	return 0;
}

#endif
