#ifndef _FLUX_LAUNCH_H
#define _FLUX_LAUNCH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum flux_launch_kind {
	FLUX_LAUNCH_NATIVE = 0,
	FLUX_LAUNCH_RUNC_RUN,
};

struct flux_launch_spec {
	enum flux_launch_kind kind;
	char *container_id;
	const char *filename;
	char **argv;
	int argc;
	char **extra_envp;
	bool owns_extra_envp;
	bool uses_oci_cfg;
};

bool flux_launch_should_bootstrap_multiproc(int argc, char **argv);
int flux_launch_bootstrap_multiproc(char *argv[]);

int flux_launch_prepare_flux_cli(struct flux_launch_spec *spec, int argc,
				 char **argv);
int flux_launch_ensure_run_cfg_path(void);
int flux_launch_prepare_runc_container(struct flux_launch_spec *spec,
				       const char *container_id,
				       const char *bundle_dir,
				       const char *config_path);
int flux_launch_prepare_runc_run(struct flux_launch_spec *spec, int argc,
				 char **argv);
int flux_launch_run(struct flux_launch_spec *spec);
void flux_launch_cleanup(struct flux_launch_spec *spec);

const struct flux_launch_spec *flux_launch_get(void);

#ifdef __cplusplus
}
#endif

#endif
