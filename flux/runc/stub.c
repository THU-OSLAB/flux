#include <flux.h>
#include <flux/runc.h>

#ifndef CONFIG_FLUX_RUNC

int flux_runc_set_bundle(const char *bundle_dir)
{
	(void)bundle_dir;
	return -FLUX_ENOSYS;
}

int flux_runc_set_bundle_config(const char *bundle_dir, const char *config_path)
{
	(void)bundle_dir;
	(void)config_path;
	return -FLUX_ENOSYS;
}

int flux_runc_load_bundle(void)
{
	return -FLUX_ENOSYS;
}

int flux_runc_prepare_container_fs(void)
{
	return -FLUX_ENOSYS;
}

int flux_runc_enter_container_fs(void)
{
	return -FLUX_ENOSYS;
}

void flux_runc_unload(void)
{
}

bool flux_runc_bundle_enabled(void)
{
	return false;
}

const struct flux_oci_cfg *flux_oci_cfg_get(void)
{
	return NULL;
}

#endif
