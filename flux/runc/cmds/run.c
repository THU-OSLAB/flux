#include <stdlib.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_run(int argc, char **argv)
{
	struct flux_runc_bundle_cmd cmd;
	struct flux_runc_state state;
	int ret;

	flux_runc_state_reset(&state);
	ret = flux_runc_parse_bundle_command(argc, argv, &cmd);
	if (ret < 0)
		return EXIT_FAILURE;

	ret = flux_runc_create_state(cmd.bundle_dir, cmd.container_id,
				     cmd.pid_file, &state);
	if (ret < 0)
		return EXIT_FAILURE;

	if (cmd.detach && state.terminal) {
		if (!cmd.console_socket || !cmd.console_socket[0]) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"run -d for terminal containers requires --console-socket\n");
			flux_runc_state_fini(&state);
			(void)flux_runc_state_remove(&state);
			return EXIT_FAILURE;
		}

		ret = flux_runc_runner_start_terminal_detached(
			&state, state.pid_file_path, cmd.console_socket);
		if (ret < 0)
			flux_runc_log_errno("failed to run container", ret);

		if (ret < 0) {
			int cleanup_ret = flux_runc_state_remove(&state);

			if (cleanup_ret < 0)
				flux_runc_log_errno(
					"failed to cleanup runtime state",
					cleanup_ret);
		}

		flux_runc_state_fini(&state);
		return ret < 0 ? EXIT_FAILURE : 0;
	}

	ret = flux_runc_run_loaded_state(&state, cmd.detach);
	if (ret < 0)
		flux_runc_log_errno("failed to run container", ret);

	if (ret < 0) {
		int cleanup_ret = flux_runc_state_remove(&state);

		if (cleanup_ret < 0)
			flux_runc_log_errno("failed to cleanup runtime state",
					    cleanup_ret);
	}

	flux_runc_state_fini(&state);

	if (ret < 0)
		return EXIT_FAILURE;

	return cmd.detach ? 0 : ret;
}
