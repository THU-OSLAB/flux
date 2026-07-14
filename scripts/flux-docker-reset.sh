#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DOCKER_HOST_URI="${DOCKER_HOST_URI:-unix:///tmp/flux-docker.sock}"
DATA_ROOT="${DATA_ROOT:-/tmp/flux-docker-data}"
EXEC_ROOT="${EXEC_ROOT:-/tmp/flux-docker-exec}"
PIDFILE="${PIDFILE:-/tmp/flux-dockerd.pid}"
SOCKFILE="${SOCKFILE:-/tmp/flux-docker.sock}"
LOGFILE="${LOGFILE:-/tmp/flux-dockerd.log}"

die() {
	echo "flux-docker-reset: $*" >&2
	exit 1
}

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

docker_alive() {
	docker -H "$DOCKER_HOST_URI" info >/dev/null 2>&1
}

stop_pidfile_process() {
	local pid

	[[ -f "$PIDFILE" ]] || return 0
	pid="$(cat "$PIDFILE" 2>/dev/null || true)"
	[[ -n "$pid" ]] || return 0
	sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
	for _ in $(seq 1 10); do
		sudo -n kill -0 "$pid" >/dev/null 2>&1 || return 0
		sleep 1
	done
	sudo -n kill -KILL "$pid" >/dev/null 2>&1 || true
}

prune_docker_state() {
	local ids=""

	if docker_alive; then
		ids="$(docker -H "$DOCKER_HOST_URI" ps -aq 2>/dev/null || true)"
		if [[ -n "$ids" ]]; then
			docker -H "$DOCKER_HOST_URI" rm -f $ids >/dev/null 2>&1 || true
		fi
		docker -H "$DOCKER_HOST_URI" system prune -af --volumes >/dev/null 2>&1 || true
	fi
}

remove_paths() {
	sudo -n rm -rf \
		"$DATA_ROOT" \
		"$EXEC_ROOT" \
		"$PIDFILE" \
		"$SOCKFILE" \
		"$LOGFILE" \
		>/dev/null 2>&1 || true
}

cleanup_orphan_runtime_tasks() {
	local line
	local pid
	local cmd
	local id
	local shim_pids
	local shim_pid

	while IFS= read -r line; do
		pid="${line%% *}"
		cmd="${line#* }"
		id="${cmd##* }"
		sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
		if [[ "$id" =~ ^[0-9a-f]{64}$ ]]; then
			while IFS= read -r shim_pid; do
				[[ -n "$shim_pid" ]] || continue
				sudo -n kill -TERM "$shim_pid" >/dev/null 2>&1 || true
			done < <(pgrep -f "containerd-shim-runc-v2 -namespace moby -id $id -address /run/containerd/containerd.sock" || true)
		fi
	done < <(pgrep -af "/tkf-workspace/flux/build/flux-runc --root $EXEC_ROOT/runtime-runc/moby" || true)
}

main() {
	need_cmd docker
	need_cmd sudo
	need_cmd pgrep

	prune_docker_state
	stop_pidfile_process
	cleanup_orphan_runtime_tasks
	remove_paths

	printf 'flux-docker-reset: cleared %s and %s\n' "$DATA_ROOT" "$EXEC_ROOT" >&2
}

main "$@"
