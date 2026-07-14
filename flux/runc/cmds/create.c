#include <stdlib.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_create(int argc, char **argv)
{
	struct flux_runc_bundle_cmd cmd;
	struct flux_runc_state state;
	int ret;

	flux_runc_state_reset(&state);
	ret = flux_runc_parse_bundle_command(argc, argv, &cmd);
	if (ret < 0)
		return EXIT_FAILURE;
	if (cmd.detach) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "create does not support --detach; use start/run\n");
		return EXIT_FAILURE;
	}

	ret = flux_runc_create_state(cmd.bundle_dir, cmd.container_id,
				     cmd.pid_file, &state);
	if (ret < 0)
		return EXIT_FAILURE;

	ret = flux_runc_create_loaded_state(&state, cmd.console_socket);
	if (ret < 0) {
		flux_runc_log_errno("failed to create container", ret);
		(void)flux_runc_state_remove(&state);
	}
	flux_runc_state_fini(&state);
	return ret < 0 ? EXIT_FAILURE : 0;
}
