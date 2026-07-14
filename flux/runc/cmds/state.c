#include <stdlib.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_state(int argc, char **argv)
{
	struct flux_runc_simple_cmd cmd;
	struct flux_runc_state state;
	int ret;

	ret = flux_runc_parse_simple_command(argc, argv, &cmd);
	if (ret < 0 || cmd.detach || cmd.force || cmd.pid_file)
		return EXIT_FAILURE;

	ret = flux_runc_state_load(&state, cmd.container_id);
	if (ret < 0) {
		flux_runc_log_errno("failed to load runtime state", ret);
		return EXIT_FAILURE;
	}

	ret = flux_runc_reconcile_loaded_state(&state,
					       "failed to reconcile runtime state");
	if (ret < 0) {
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_runc_state_print(&state, stdout);
	flux_runc_state_fini(&state);
	return ret < 0 ? EXIT_FAILURE : 0;
}
