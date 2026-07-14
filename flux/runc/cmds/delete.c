#include <errno.h>
#include <signal.h>
#include <stdlib.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_delete(int argc, char **argv)
{
	struct flux_runc_simple_cmd cmd;
	struct flux_runc_state state;
	bool live_created;
	int ret;

	ret = flux_runc_parse_simple_command(argc, argv, &cmd);
	if (ret < 0 || cmd.detach || cmd.pid_file)
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

	live_created = state.status == FLUX_RUNC_CREATED && state.init_pid > 0;

	if (state.status == FLUX_RUNC_RUNNING && !cmd.force) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "refusing to delete running container %s without --force\n",
			 state.id);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	if ((state.status == FLUX_RUNC_RUNNING && cmd.force) || live_created) {
		ret = flux_runc_runner_force_kill(&state);
		if (ret < 0 && ret != -ESRCH) {
			flux_runc_log_errno("failed to force kill container",
					    ret);
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}

		ret = flux_runc_runner_wait_for_exit(&state, 5000);
		if (ret < 0 && ret != -ETIMEDOUT) {
			flux_runc_log_errno("failed waiting for forced delete", ret);
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}

		ret = flux_runc_reconcile_loaded_state(
			&state, "failed to reconcile runtime state");
		if (ret < 0) {
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}
	}

	if (state.status == FLUX_RUNC_RUNNING ||
	    (state.status == FLUX_RUNC_CREATED && state.init_pid > 0)) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "container %s did not stop before delete\n", state.id);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_runc_state_cleanup_artifacts(&state);
	if (ret < 0) {
		flux_runc_log_errno("failed to cleanup runtime artifacts", ret);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_runc_state_remove(&state);
	if (ret < 0)
		flux_runc_log_errno("failed to remove runtime state", ret);

	flux_runc_state_fini(&state);
	return ret < 0 ? EXIT_FAILURE : 0;
}
