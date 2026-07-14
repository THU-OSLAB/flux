#include <stdlib.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_start(int argc, char **argv)
{
	struct flux_runc_simple_cmd cmd;
	struct flux_runc_state state;
	int exit_code = EXIT_FAILURE;
	bool waited_foreground = false;
	int ret;

	flux_runc_state_reset(&state);
	ret = flux_runc_parse_simple_command(argc, argv, &cmd);
	if (ret < 0 || cmd.force)
		return EXIT_FAILURE;

	if (cmd.detach) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"start does not support --detach; start already returns after the container has launched\n");
		return EXIT_FAILURE;
	}

	ret = flux_runc_state_load(&state, cmd.container_id);
	if (ret < 0) {
		flux_runc_log_errno("failed to load runtime state", ret);
		return EXIT_FAILURE;
	}

	ret = flux_runc_reconcile_loaded_state(
		&state, "failed to reconcile runtime state");
	if (ret < 0) {
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	if (state.status != FLUX_RUNC_CREATED) {
		FLUX_LOG(FLUX_LOG_ERR, "container %s is not in created state\n",
			 state.id);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	if (cmd.pid_file) {
		ret = flux_runc_state_set_pid_file(&state, cmd.pid_file);
		if (ret < 0) {
			flux_runc_log_errno("failed to update pid file path",
					    ret);
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}
		ret = flux_runc_state_save(&state);
		if (ret < 0) {
			flux_runc_log_errno("failed to update pid file path",
					    ret);
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}
	}

	if (state.init_pid > 0)
		ret = flux_runc_start_loaded_state(&state);
	else if (state.terminal && cmd.console_socket) {
		ret = flux_runc_runner_start_terminal_detached(
			&state, state.pid_file_path, cmd.console_socket);
	} else if (state.terminal) {
		ret = flux_runc_start_loaded_state(&state);
	} else
		ret = flux_runc_start_loaded_state(&state);
	if (ret < 0)
		flux_runc_log_errno("failed to start container", ret);

	flux_runc_state_fini(&state);
	if (ret < 0)
		return EXIT_FAILURE;

	return waited_foreground ? exit_code : 0;
}
