#define FLUX_FMT "flux: "

#include <string.h>
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

	if (oci->user.has_gid) {
		ret = flux_sys_setgroups(0, NULL);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "OCI setgroups(0) failed: %s\n",
				 flux_strerror(-ret));
			return ret;
		}
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

	if (oci->hostname) {
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

	err = flux_apply_oci_user(oci);
	if (err < 0)
		return err;

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

	err = flux_apply_oci_rlimits(oci);
	if (err < 0)
		return err;

	if (oci->no_new_privileges) {
		err = flux_sys_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		if (err < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "OCI PR_SET_NO_NEW_PRIVS failed: %s\n",
				 flux_strerror(-err));
			return err;
		}
	}

	return 0;
}

#endif
