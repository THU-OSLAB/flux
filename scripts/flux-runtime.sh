#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DEFAULT_RUN_CFG="$ROOT_DIR/configs/config.json"
DEFAULT_IOKD_CFG="$ROOT_DIR/configs/config_iokd.json"
START_TIMEOUT_SECS="${FLUX_IOKD_START_TIMEOUT_SECS:-15}"

usage() {
	cat <<'EOF'
Usage:
  scripts/flux-runtime.sh build
  scripts/flux-runtime.sh resolve-sock [--run-cfg FILE] [--iokd-cfg FILE]
  scripts/flux-runtime.sh check-iokd [--run-cfg FILE] [--iokd-cfg FILE]
  scripts/flux-runtime.sh ensure-iokd [--run-cfg FILE] [--iokd-cfg FILE]
  scripts/flux-runtime.sh flux [--run-cfg FILE] [--iokd-cfg FILE] [--no-start] [args...]
  scripts/flux-runtime.sh flux-runc [--run-cfg FILE] [--iokd-cfg FILE] [--no-start] [args...]

Commands:
  build         Run the required build with UINTR=1 FNET=1 FAST_NET=1.
  resolve-sock  Print the socket path this user-scoped run should target.
  check-iokd    Exit 0 only if a healthy flux-iokd is serving that exact socket.
  ensure-iokd   Start flux-iokd for that socket if needed, then verify it.
  flux          Ensure flux-iokd, export FLUX_RUN_CFG_FILE, then exec build/flux.
  flux-runc     Ensure flux-iokd, export FLUX_RUN_CFG_FILE, then exec build/flux-runc.
EOF
}

die() {
	echo "flux-runtime: $*" >&2
	exit 1
}

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

owner_uid() {
	if [[ -n "${SUDO_UID:-}" ]]; then
		printf '%s\n' "$SUDO_UID"
	else
		id -u
	fi
}

owner_name() {
	local uid
	local entry

	uid="$(owner_uid)"
	entry="$(getent passwd "$uid" 2>/dev/null || true)"
	if [[ -n "$entry" ]]; then
		printf '%s\n' "${entry%%:*}"
	else
		printf '%s\n' "$uid"
	fi
}

default_sock_path() {
	printf '/tmp/flux.sock.%s\n' "$(owner_name)"
}

abs_path() {
	local path="$1"

	if [[ -z "$path" ]]; then
		return 1
	fi
	if [[ "$path" = /* ]]; then
		printf '%s\n' "$path"
	else
		printf '%s\n' "$ROOT_DIR/$path"
	fi
}

json_string_field() {
	local file="$1"
	local key="$2"

	[[ -f "$file" ]] || return 0
	need_cmd jq
	jq -r --arg key "$key" '
		if type == "object" then
			.[$key] // empty
		else
			empty
		end
	' "$file"
}

resolve_sock_path() {
	local run_cfg="$1"
	local iokd_cfg="$2"
	local path=""

	if [[ -n "$run_cfg" ]]; then
		path="$(json_string_field "$run_cfg" "iok_sock_path")"
	fi
	if [[ -z "$path" && -n "$iokd_cfg" ]]; then
		path="$(json_string_field "$iokd_cfg" "sock_path")"
	fi
	if [[ -z "$path" ]]; then
		path="$(default_sock_path)"
	fi
	printf '%s\n' "$path"
}

socket_listener_alive() {
	local socket_path="$1"

	[[ -S "$socket_path" ]] || return 1

	if command -v ss >/dev/null 2>&1; then
		ss -xlH | awk '{print $5}' | grep -Fx -- "$socket_path" >/dev/null 2>&1
		return $?
	fi

	# Avoid probing the control socket with connect(): flux-iokd treats every
	# accepted connection as the start of an attach handshake, so bare connects
	# create noisy invalid-attach failures and can perturb validation runs.
	return 0
}

start_iokd() {
	local socket_path="$1"
	local iokd_cfg="$2"
	local log_file
	local cmd=("$BUILD_DIR/flux-iokd")
	local pid=""
	local deadline

	[[ -x "$BUILD_DIR/flux-iokd" ]] || die "missing binary: $BUILD_DIR/flux-iokd"

	if [[ -n "$iokd_cfg" && -f "$iokd_cfg" ]]; then
		cmd+=(-c "$iokd_cfg")
	fi
	cmd+=(--sock-path "$socket_path")

	log_file="${TMPDIR:-/tmp}/flux-iokd-$(owner_name).log"
	"${cmd[@]}" >"$log_file" 2>&1 &
	pid=$!
	deadline=$((SECONDS + START_TIMEOUT_SECS))

	while (( SECONDS < deadline )); do
		if socket_listener_alive "$socket_path"; then
			printf 'flux-runtime: started flux-iokd pid=%s socket=%s log=%s\n' \
				"$pid" "$socket_path" "$log_file" >&2
			return 0
		fi
		if ! kill -0 "$pid" >/dev/null 2>&1; then
			tail -n 40 "$log_file" >&2 || true
			die "flux-iokd exited before serving $socket_path"
		fi
		sleep 1
	done

	tail -n 40 "$log_file" >&2 || true
	die "timed out waiting for flux-iokd to serve $socket_path"
}

ensure_iokd() {
	local run_cfg="$1"
	local iokd_cfg="$2"
	local allow_start="$3"
	local socket_path

	socket_path="$(resolve_sock_path "$run_cfg" "$iokd_cfg")"
	if socket_listener_alive "$socket_path"; then
		printf 'flux-runtime: healthy flux-iokd found on %s\n' "$socket_path" >&2
		printf '%s\n' "$socket_path"
		return 0
	fi

	if [[ "$allow_start" != "true" ]]; then
		die "no healthy flux-iokd is serving $socket_path"
	fi

	start_iokd "$socket_path" "$iokd_cfg"
	printf '%s\n' "$socket_path"
}

read_first_line() {
	local file="$1"
	local value=""

	[[ -r "$file" ]] || return 1
	IFS= read -r value <"$file" || return 1
	printf '%s\n' "$value"
}

cpu_is_online() {
	local cpu_dir="$1"
	local online_file="$cpu_dir/online"

	if [[ ! -e "$online_file" ]]; then
		return 0
	fi
	[[ "$(read_first_line "$online_file" 2>/dev/null || true)" == "1" ]]
}

cpu_core_key() {
	local cpu_dir="$1"
	local siblings=""
	local package_id=""
	local core_id=""
	local cpu_name

	siblings="$(read_first_line "$cpu_dir/topology/thread_siblings_list" 2>/dev/null || true)"
	if [[ -n "$siblings" ]]; then
		printf '%s\n' "$siblings"
		return 0
	fi

	package_id="$(read_first_line "$cpu_dir/topology/physical_package_id" 2>/dev/null || true)"
	core_id="$(read_first_line "$cpu_dir/topology/core_id" 2>/dev/null || true)"
	if [[ -n "$package_id" && -n "$core_id" ]]; then
		printf '%s:%s\n' "$package_id" "$core_id"
		return 0
	fi

	cpu_name="${cpu_dir##*/}"
	printf '%s\n' "${cpu_name#cpu}"
}

cpu_core_type_is_performance() {
	local value="$1"
	local normalized="${value,,}"

	case "$normalized" in
	64|40|0x40|00000040|0x00000040|intelcore|intel_core|intel-core|intel\ core|performance|pcore|p-core)
		return 0
		;;
	esac

	return 1
}

detect_perf_core_jobs_from_core_type() {
	local cpu_dir
	local key=""
	local core_type=""
	local -A seen_cores=()
	local -A seen_types=()
	local perf_cores=0

	for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
		[[ -d "$cpu_dir" ]] || continue
		cpu_is_online "$cpu_dir" || continue
		key="$(cpu_core_key "$cpu_dir")"
		[[ -n "${seen_cores[$key]:-}" ]] && continue
		seen_cores["$key"]=1

		core_type="$(read_first_line "$cpu_dir/topology/core_type" 2>/dev/null || true)"
		if [[ -z "$core_type" ]]; then
			return 1
		fi
		seen_types["$core_type"]=1
		if cpu_core_type_is_performance "$core_type"; then
			((perf_cores += 1))
		fi
	done

	if (( ${#seen_cores[@]} == 0 || ${#seen_types[@]} < 2 || perf_cores == 0 )); then
		return 1
	fi

	printf '%s\n' "$perf_cores"
}

detect_perf_core_jobs_from_capacity() {
	local cpu_dir
	local key=""
	local capacity=""
	local max_capacity=-1
	local perf_cores=0
	local -A seen_cores=()
	local -A capacity_counts=()

	for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
		[[ -d "$cpu_dir" ]] || continue
		cpu_is_online "$cpu_dir" || continue
		key="$(cpu_core_key "$cpu_dir")"
		[[ -n "${seen_cores[$key]:-}" ]] && continue
		seen_cores["$key"]=1

		capacity="$(read_first_line "$cpu_dir/cpu_capacity" 2>/dev/null || true)"
		if [[ ! "$capacity" =~ ^[0-9]+$ ]]; then
			return 1
		fi
		capacity_counts["$capacity"]=$(( ${capacity_counts["$capacity"]:-0} + 1 ))
		if (( capacity > max_capacity )); then
			max_capacity="$capacity"
		fi
	done

	if (( ${#seen_cores[@]} == 0 || ${#capacity_counts[@]} < 2 || max_capacity < 0 )); then
		return 1
	fi

	perf_cores="${capacity_counts["$max_capacity"]}"
	if (( perf_cores == 0 )); then
		return 1
	fi

	printf '%s\n' "$perf_cores"
}

detect_perf_core_jobs_from_max_freq() {
	local cpu_dir
	local key=""
	local max_freq=""
	local best_freq=-1
	local perf_cores=0
	local -A seen_cores=()
	local -A freq_counts=()

	for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
		[[ -d "$cpu_dir" ]] || continue
		cpu_is_online "$cpu_dir" || continue
		key="$(cpu_core_key "$cpu_dir")"
		[[ -n "${seen_cores[$key]:-}" ]] && continue
		seen_cores["$key"]=1

		max_freq="$(read_first_line "$cpu_dir/cpufreq/cpuinfo_max_freq" 2>/dev/null || true)"
		if [[ ! "$max_freq" =~ ^[0-9]+$ ]]; then
			max_freq="$(read_first_line "$cpu_dir/cpufreq/scaling_max_freq" 2>/dev/null || true)"
		fi
		if [[ ! "$max_freq" =~ ^[0-9]+$ ]]; then
			return 1
		fi
		freq_counts["$max_freq"]=$(( ${freq_counts["$max_freq"]:-0} + 1 ))
		if (( max_freq > best_freq )); then
			best_freq="$max_freq"
		fi
	done

	if (( ${#seen_cores[@]} == 0 || ${#freq_counts[@]} < 2 || best_freq < 0 )); then
		return 1
	fi

	perf_cores="${freq_counts["$best_freq"]}"
	if (( perf_cores == 0 )); then
		return 1
	fi

	printf '%s\n' "$perf_cores"
}

build_jobs() {
	local jobs=""

	# Prefer physical performance-core count on hybrid CPUs.
	jobs="$(detect_perf_core_jobs_from_core_type 2>/dev/null || true)"
	if [[ -n "$jobs" ]]; then
		printf '%s\n' "$jobs"
		return 0
	fi

	jobs="$(detect_perf_core_jobs_from_capacity 2>/dev/null || true)"
	if [[ -n "$jobs" ]]; then
		printf '%s\n' "$jobs"
		return 0
	fi

	jobs="$(detect_perf_core_jobs_from_max_freq 2>/dev/null || true)"
	if [[ -n "$jobs" ]]; then
		printf '%s\n' "$jobs"
		return 0
	fi

	if command -v nproc >/dev/null 2>&1; then
		nproc
		return 0
	fi

	getconf _NPROCESSORS_ONLN
}

run_build() {
	local jobs

	need_cmd make
	jobs="$(build_jobs)"
	printf 'flux-runtime: building with %s job(s)\n' "$jobs" >&2
	(
		cd "$ROOT_DIR"
		UINTR=1 FNET=1 FAST_NET=1 make -j"$jobs"
	)
}

parse_shared_args() {
	RUN_CFG="$DEFAULT_RUN_CFG"
	IOKD_CFG=""
	ALLOW_START="true"
	PASSTHRU_ARGS=()

	if [[ -f "$DEFAULT_IOKD_CFG" ]]; then
		IOKD_CFG="$DEFAULT_IOKD_CFG"
	fi

	while [[ $# -gt 0 ]]; do
		case "$1" in
		--run-cfg)
			[[ $# -ge 2 ]] || die "--run-cfg requires a path"
			RUN_CFG="$(abs_path "$2")"
			shift 2
			;;
		--iokd-cfg)
			[[ $# -ge 2 ]] || die "--iokd-cfg requires a path"
			IOKD_CFG="$(abs_path "$2")"
			shift 2
			;;
		--no-start)
			ALLOW_START="false"
			shift
			;;
		--)
			shift
			PASSTHRU_ARGS=("$@")
			return 0
			;;
		*)
			PASSTHRU_ARGS=("$@")
			return 0
			;;
		esac
	done
}

main() {
	local command="${1:-}"
	local socket_path

	[[ -n "$command" ]] || {
		usage
		exit 1
	}
	shift

	case "$command" in
	build)
		run_build
		;;
	resolve-sock)
		parse_shared_args "$@"
		printf '%s\n' "$(resolve_sock_path "$RUN_CFG" "$IOKD_CFG")"
		;;
	check-iokd)
		parse_shared_args "$@"
		socket_path="$(resolve_sock_path "$RUN_CFG" "$IOKD_CFG")"
		if socket_listener_alive "$socket_path"; then
			printf 'flux-runtime: healthy flux-iokd found on %s\n' "$socket_path" >&2
			exit 0
		fi
		die "no healthy flux-iokd is serving $socket_path"
		;;
	ensure-iokd)
		parse_shared_args "$@"
		ensure_iokd "$RUN_CFG" "$IOKD_CFG" "$ALLOW_START" >/dev/null
		;;
	flux)
		parse_shared_args "$@"
		[[ -x "$BUILD_DIR/flux" ]] || die "missing binary: $BUILD_DIR/flux"
		ensure_iokd "$RUN_CFG" "$IOKD_CFG" "$ALLOW_START" >/dev/null
		exec env FLUX_RUN_CFG_FILE="$RUN_CFG" "$BUILD_DIR/flux" "${PASSTHRU_ARGS[@]}"
		;;
	flux-runc)
		parse_shared_args "$@"
		[[ -x "$BUILD_DIR/flux-runc" ]] || die "missing binary: $BUILD_DIR/flux-runc"
		ensure_iokd "$RUN_CFG" "$IOKD_CFG" "$ALLOW_START" >/dev/null
		exec env FLUX_RUN_CFG_FILE="$RUN_CFG" "$BUILD_DIR/flux-runc" "${PASSTHRU_ARGS[@]}"
		;;
	-h|--help|help)
		usage
		;;
	*)
		usage
		die "unknown command: $command"
		;;
	esac
}

main "$@"
