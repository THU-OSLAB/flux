#include <errno.h>
#include <stdlib.h>

#include <flux.h>

#include "../runc.h"

int flux_runc_cmd_kill(int argc, char **argv)
{
	struct flux_runc_kill_cmd cmd;
	struct flux_runc_state state;
	int signo;
	int ret;

	ret = flux_runc_parse_kill_command(argc, argv, &cmd);
	if (ret < 0)
		return EXIT_FAILURE;

	ret = flux_runc_parse_signal_spec(cmd.signal_spec, &signo);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "invalid signal '%s'\n",
			 cmd.signal_spec ?: "");
		return EXIT_FAILURE;
	}
	if (!flux_runc_signal_is_bridgeable(signo)) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "signal %d is not supported by the runtime control plane\n",
			 signo);
		return EXIT_FAILURE;
	}

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

	if (state.status != FLUX_RUNC_RUNNING) {
		FLUX_LOG(FLUX_LOG_ERR, "container %s is not running\n", state.id);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	ret = flux_runc_runner_signal(&state, signo);
	if (ret < 0 && ret != -ESRCH) {
		flux_runc_log_errno("failed to signal container", ret);
		flux_runc_state_fini(&state);
		return EXIT_FAILURE;
	}

	if (ret == 0 && flux_runc_signal_may_change_lifecycle(signo)) {
		ret = flux_runc_runner_wait_for_exit(&state, 1000);
		if (ret < 0 && ret != -ETIMEDOUT) {
			flux_runc_log_errno("failed to observe container after signal",
					    ret);
			flux_runc_state_fini(&state);
			return EXIT_FAILURE;
		}
	}

	ret = flux_runc_reconcile_loaded_state(&state,
					       "failed to reconcile runtime state");
	flux_runc_state_fini(&state);
	return ret < 0 ? EXIT_FAILURE : 0;
}
