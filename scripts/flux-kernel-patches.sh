#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
FLUX_ROOT="${FLUX_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd -P)}"
PATCH_DIR="${FLUX_KERNEL_PATCH_DIR:-$FLUX_ROOT/scripts/patches/flux-kernel}"

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
Usage:
  scripts/flux-kernel-patches.sh check
  scripts/flux-kernel-patches.sh apply
  scripts/flux-kernel-patches.sh reverse
  scripts/flux-kernel-patches.sh manifest
  scripts/flux-kernel-patches.sh run -- COMMAND [ARG ...]

The patch series is read in bytewise filename order.  "run" applies the whole
series, executes COMMAND, and restores the original tracked source state even
when COMMAND fails or receives a signal.
EOF
}

for command in awk find git sha256sum sort; do
	command -v "$command" >/dev/null 2>&1 || die "required command not found: $command"
done
git -C "$FLUX_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
	die "not a Git worktree: $FLUX_ROOT"
[[ -d "$PATCH_DIR" ]] || die "patch directory not found: $PATCH_DIR"

mapfile -d '' -t PATCHES < <(
	find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' -print0 |
		LC_ALL=C sort -z
)
((${#PATCHES[@]} > 0)) || die "no patches found in $PATCH_DIR"

declare -a APPLIED=()
declare -a REVERSED=()

relative_patch() {
	local patch=$1
	case "$patch" in
	"$FLUX_ROOT"/*) printf '%s\n' "${patch#"$FLUX_ROOT/"}" ;;
	*) printf '%s\n' "$patch" ;;
	esac
}

tracked_state_sha256() {
	git -C "$FLUX_ROOT" diff --binary --full-index --no-ext-diff HEAD -- |
		sha256sum | awk '{print $1}'
}

rollback_applied() {
	local i patch failed=0
	for ((i=${#APPLIED[@]} - 1; i >= 0; i--)); do
		patch=${APPLIED[$i]}
		if ! git -C "$FLUX_ROOT" apply --reverse --check "$patch"; then
			printf 'error: cannot roll back patch: %s\n' "$(relative_patch "$patch")" >&2
			failed=1
			continue
		fi
		git -C "$FLUX_ROOT" apply --reverse "$patch" || failed=1
	done
	APPLIED=()
	return "$failed"
}

rollback_reversed() {
	local i patch failed=0
	# REVERSED is populated from the end of the series.  Reapply its entries
	# backwards to recover the original ascending dependency order.
	for ((i=${#REVERSED[@]} - 1; i >= 0; i--)); do
		patch=${REVERSED[$i]}
		if ! git -C "$FLUX_ROOT" apply --check "$patch"; then
			printf 'error: cannot restore reversed patch: %s\n' "$(relative_patch "$patch")" >&2
			failed=1
			continue
		fi
		git -C "$FLUX_ROOT" apply "$patch" || failed=1
	done
	REVERSED=()
	return "$failed"
}

apply_series() {
	local patch
	APPLIED=()
	for patch in "${PATCHES[@]}"; do
		if ! git -C "$FLUX_ROOT" apply --check "$patch"; then
			printf 'error: patch does not apply cleanly: %s\n' "$(relative_patch "$patch")" >&2
			rollback_applied || true
			return 1
		fi
		if ! git -C "$FLUX_ROOT" apply "$patch"; then
			printf 'error: failed to apply patch: %s\n' "$(relative_patch "$patch")" >&2
			rollback_applied || true
			return 1
		fi
		APPLIED+=("$patch")
		printf 'applied\t%s\n' "$(relative_patch "$patch")"
	done
}

reverse_series() {
	local i patch
	REVERSED=()
	for ((i=${#PATCHES[@]} - 1; i >= 0; i--)); do
		patch=${PATCHES[$i]}
		if ! git -C "$FLUX_ROOT" apply --reverse --check "$patch"; then
			printf 'error: applied patch cannot be reversed cleanly: %s\n' \
				"$(relative_patch "$patch")" >&2
			rollback_reversed || true
			return 1
		fi
		if ! git -C "$FLUX_ROOT" apply --reverse "$patch"; then
			printf 'error: failed to reverse patch: %s\n' "$(relative_patch "$patch")" >&2
			rollback_reversed || true
			return 1
		fi
		REVERSED+=("$patch")
		printf 'reversed\t%s\n' "$(relative_patch "$patch")"
	done
}

run_with_patches() {
	local before after rc=0 patches_active=0
	before=$(tracked_state_sha256)

	cleanup_run() {
		local cleanup_rc=$?
		trap - EXIT HUP INT TERM
		if ((patches_active)); then
			rollback_applied || cleanup_rc=1
		fi
		after=$(tracked_state_sha256)
		if [[ "$after" != "$before" ]]; then
			printf 'error: tracked source state changed while patches were active\n' >&2
			cleanup_rc=1
		fi
		exit "$cleanup_rc"
	}

	trap cleanup_run EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
	patches_active=1
	apply_series
	cd "$FLUX_ROOT"
	"$@" || rc=$?
	exit "$rc"
}

apply_persistent() {
	local complete=0

	cleanup_apply() {
		local cleanup_rc=$?
		trap - EXIT HUP INT TERM
		if ((!complete)); then
			rollback_applied || cleanup_rc=1
		fi
		exit "$cleanup_rc"
	}

	trap cleanup_apply EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
	apply_series
	complete=1
	APPLIED=()
	trap - EXIT HUP INT TERM
}

reverse_persistent() {
	local complete=0

	cleanup_reverse() {
		local cleanup_rc=$?
		trap - EXIT HUP INT TERM
		if ((!complete)); then
			rollback_reversed || cleanup_rc=1
		fi
		exit "$cleanup_rc"
	}

	trap cleanup_reverse EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
	reverse_series
	complete=1
	REVERSED=()
	trap - EXIT HUP INT TERM
}

mode=${1:-}
case "$mode" in
check)
	[[ $# -eq 1 ]] || { usage >&2; exit 64; }
	run_with_patches true
	;;
apply)
	[[ $# -eq 1 ]] || { usage >&2; exit 64; }
	apply_persistent
	;;
reverse)
	[[ $# -eq 1 ]] || { usage >&2; exit 64; }
	reverse_persistent
	;;
manifest)
	[[ $# -eq 1 ]] || { usage >&2; exit 64; }
	for patch in "${PATCHES[@]}"; do
		printf '%s\t%s\n' "$(sha256sum "$patch" | awk '{print $1}')" \
			"$(relative_patch "$patch")"
	done
	;;
run)
	shift
	[[ ${1:-} == -- ]] && shift
	[[ $# -gt 0 ]] || { usage >&2; exit 64; }
	run_with_patches "$@"
	;;
-h | --help)
	usage
	;;
*)
	usage >&2
	exit 64
	;;
esac
