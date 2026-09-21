#ifndef _FLUX_RUNC_INTERNAL_H
#define _FLUX_RUNC_INTERNAL_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#include <flux/runc.h>
#include <kernel/asm/flux_oci.h>

struct flux_resource_stats;

enum flux_runc_status {
	FLUX_RUNC_CREATING = 0,
	FLUX_RUNC_CREATED,
	FLUX_RUNC_RUNNING,
	FLUX_RUNC_STOPPED,
};

struct flux_runc_state {
	char *id;
	char *bundle_dir;
	char *state_root;
	char *state_dir;
	char *oci_config_path;
	char *run_config_path;
	char *resources_path;
	char *pid_file_path;
	char *exec_ring_name;
	char *cgroup_path;
	pid_t init_pid;
	int exit_code;
	enum flux_runc_status status;
	bool terminal;
};

struct flux_runc_bundle_cmd {
	const char *bundle_dir;
	const char *console_socket;
	const char *pid_file;
	const char *container_id;
	bool detach;
};

struct flux_runc_simple_cmd {
	const char *pid_file;
	const char *console_socket;
	const char *container_id;
	bool all;
	bool detach;
	bool force;
};

struct flux_runc_kill_cmd {
	const char *container_id;
	const char *signal_spec;
};

struct flux_runc_exec_cmd {
	const char *container_id;
	const char *process_path;
	const char *pid_file;
	const char *console_socket;
	const char *cwd;
	const char *user_spec;
	char **env;
	int env_count;
	char **argv;
	int argc;
	bool detach;
};

struct flux_runc_exec_spec {
	char *filename;
	char *cwd;
	char **argv;
	int argc;
	char **env;
	int env_count;
	uid_t uid;
	gid_t gid;
	uint32_t session_id;
	unsigned int console_width;
	unsigned int console_height;
	bool has_console_size;
	bool has_user;
	bool terminal;
};

struct flux_runc_iok_stats {
	int client_id;
	unsigned int io_weight;
	unsigned int net_class_id;
	unsigned int net_priority;
	unsigned int cpu_shares;
	int64_t cpu_quota;
	uint64_t cpu_period;
	unsigned int nr_cpus;
	int cpu_list[CONFIG_FLUX_MAX_CPUS];
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t tx_packets;
	uint64_t tx_bytes;
};

const char *flux_runc_status_name(enum flux_runc_status status);
void flux_runc_state_reset(struct flux_runc_state *state);
void flux_runc_state_fini(struct flux_runc_state *state);
void flux_runc_log_errno(const char *what, int err);
void flux_runc_set_error(const char *fmt, ...)
	__attribute__((__format__(__printf__, 1, 2)));
const char *flux_runc_last_error(void);
int flux_runc_state_set_pid_file(struct flux_runc_state *state,
				 const char *pid_file);
int flux_runc_state_set_cgroup_path(struct flux_runc_state *state,
				    const char *cgroup_path);
int flux_runc_state_prepare_new(struct flux_runc_state *state, const char *id,
				const char *bundle_dir, bool terminal);
int flux_runc_state_load(struct flux_runc_state *state, const char *id);
int flux_runc_state_save(const struct flux_runc_state *state);
int flux_runc_state_reconcile(struct flux_runc_state *state);
int flux_runc_state_snapshot_bundle_config(const struct flux_runc_state *state);
int flux_runc_state_snapshot_run_config(const struct flux_runc_state *state,
					const char *path);
int flux_runc_state_save_resources(const struct flux_runc_state *state,
				   const struct flux_oci_resources *resources);
int flux_runc_state_load_resources(const struct flux_runc_state *state,
				   struct flux_oci_resources *resources);
int flux_runc_state_exec_session_root(const struct flux_runc_state *state,
				      char *buf, size_t size);
int flux_runc_state_control_pid(const struct flux_runc_state *state,
				pid_t *pid_out);
int flux_runc_state_cleanup_artifacts(const struct flux_runc_state *state);
int flux_runc_state_remove(const struct flux_runc_state *state);
int flux_runc_state_print(const struct flux_runc_state *state, FILE *stream);
int flux_runc_state_init_exec_ring(struct flux_runc_state *state);
int flux_runc_cgroup_create(const struct flux_runc_state *state);
int flux_runc_cgroup_apply(const struct flux_runc_state *state,
			   const struct flux_oci_resources *resources);
int flux_runc_cgroup_preflight(const struct flux_runc_state *state,
			       const struct flux_oci_resources *resources);
int flux_runc_cgroup_join(const struct flux_runc_state *state, pid_t pid);
int flux_runc_cgroup_destroy(const struct flux_runc_state *state);
int flux_runc_cgroup_stats(const struct flux_runc_state *state, FILE *stream);
void flux_oci_resources_fini(struct flux_oci_resources *resources);
int flux_oci_resources_copy(struct flux_oci_resources *dst,
			    const struct flux_oci_resources *src);
int flux_oci_resources_merge(struct flux_oci_resources *dst,
			     const struct flux_oci_resources *updates);
int flux_oci_resources_parse_json(const char *json,
				  struct flux_oci_resources *resources);
int flux_oci_resources_emit_json(const struct flux_oci_resources *resources,
				 FILE *stream);
int flux_runc_resources_apply_run_config(
	const struct flux_runc_state *state,
	const struct flux_oci_resources *resources);
int flux_runc_resources_update_live(
	const struct flux_runc_state *state,
	const struct flux_oci_resources *resources);
int flux_runc_resources_stats_live(
	const struct flux_runc_state *state,
	struct flux_resource_stats *stats);
int flux_runc_resources_update_iokd(
	const struct flux_runc_state *state,
	const struct flux_oci_resources *resources);
int flux_runc_resources_stats_iokd(
	const struct flux_runc_state *state,
	struct flux_runc_iok_stats *stats);
int flux_runc_create_state(const char *bundle_dir, const char *id,
			   const char *pid_file, struct flux_runc_state *state);
int flux_runc_create_loaded_state(struct flux_runc_state *state,
				  const char *console_socket);
int flux_runc_reconcile_loaded_state(struct flux_runc_state *state,
				     const char *what);
int flux_runc_start_loaded_state(struct flux_runc_state *state);
int flux_runc_run_loaded_state(struct flux_runc_state *state, bool detach);

void flux_runc_console_detach_stdio_if_needed(bool terminal);
int flux_runc_console_open_detached_pty(int *master_fd, int *slave_fd);
int flux_runc_console_set_size(int fd, unsigned int width,
			       unsigned int height);
void flux_runc_console_setup_detached_terminal(int master_fd, int slave_fd);
int flux_runc_console_send_fd(const char *socket_path, int fd);

int flux_runc_runner_exec(const char *container_id, char **argv);
int flux_runc_runner_create_background(struct flux_runc_state *state,
				       const char *pid_file);
int flux_runc_runner_create_terminal_detached(struct flux_runc_state *state,
					     const char *pid_file,
					     const char *console_socket);
int flux_runc_runner_start_background(struct flux_runc_state *state,
				      const char *pid_file);
int flux_runc_runner_start_terminal_detached(struct flux_runc_state *state,
					     const char *pid_file,
					     const char *console_socket);
int flux_runc_runner_start_foreground(struct flux_runc_state *state,
				      const char *pid_file, int *exit_code);
int flux_runc_runner_continue(struct flux_runc_state *state);
bool flux_runc_signal_is_bridgeable(int sig);
bool flux_runc_signal_may_change_lifecycle(int sig);
int flux_runc_runner_signal(const struct flux_runc_state *state, int sig);
int flux_runc_runner_resource_signal(const struct flux_runc_state *state);
int flux_runc_runner_force_kill(const struct flux_runc_state *state);
int flux_runc_runner_wait_for_exit(struct flux_runc_state *state,
				   int timeout_ms);
int flux_runc_exec_ring_create(const char *name);
int flux_runc_exec_ring_open(const char *name, bool create, bool fixed_addr,
			     struct flux_exec_ring **ring_out, int *fd_out);
void flux_runc_exec_ring_close(struct flux_exec_ring *ring, int fd);

int flux_runc_parse_bundle_command(int argc, char **argv,
				   struct flux_runc_bundle_cmd *cmd);
int flux_runc_parse_simple_command(int argc, char **argv,
				   struct flux_runc_simple_cmd *cmd);
int flux_runc_parse_kill_command(int argc, char **argv,
				 struct flux_runc_kill_cmd *cmd);
int flux_runc_parse_exec_command(int argc, char **argv,
				 struct flux_runc_exec_cmd *cmd);
int flux_runc_parse_signal_spec(const char *spec, int *signo);
int flux_runc_cmd_create(int argc, char **argv);
int flux_runc_cmd_state(int argc, char **argv);
int flux_runc_cmd_delete(int argc, char **argv);
int flux_runc_cmd_start(int argc, char **argv);
int flux_runc_cmd_run(int argc, char **argv);
int flux_runc_cmd_kill(int argc, char **argv);
int flux_runc_cmd_exec(int argc, char **argv);
int flux_runc_cmd_features(int argc, char **argv);
int flux_runc_cmd_ps(int argc, char **argv);
int flux_runc_cmd_list(int argc, char **argv);
int flux_runc_cmd_update(int argc, char **argv);
int flux_runc_cmd_events(int argc, char **argv);
int flux_runc_cmd_stats(int argc, char **argv);
int flux_runc_stats_emit(const char *container_id, FILE *stream);
int flux_runc_dispatch(int argc, char **argv);

#endif /* _FLUX_RUNC_INTERNAL_H */
