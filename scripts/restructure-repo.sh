#!/usr/bin/env bash

set -euo pipefail
shopt -s dotglob nullglob

usage() {
  cat <<'EOF'
Usage:
  scripts/restructure-repo.sh [--apply]

Behavior:
  1. Move every top-level entry in the repository root except .git into a new
     top-level wrapper directory named kernel/.
  2. Flatten kernel/tools/flux/* back into the repository root without adding
     an extra flux/ wrapper.
  3. Finalize by moving the staged kernel tree from .flux-restructure-tmp/ to
     the repository root as kernel/.

Notes:
  - Default mode is dry-run.
  - This is a literal migration. It will also move top-level workspace files
    and directories, not only tracked kernel sources.
  - If a previous run already flattened tools/flux/, this script can resume
    from .flux-restructure-tmp/kernel and finish the move.
EOF
}

APPLY=0

case "${1:-}" in
  "")
    ;;
  --apply)
    APPLY=1
    ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    usage
    exit 1
    ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
FLUX_DIR="$REPO_ROOT/tools/flux"
TMP_ROOT="$REPO_ROOT/.flux-restructure-tmp"
WRAPPER_NAME="kernel"
WRAPPER_DIR="$TMP_ROOT/$WRAPPER_NAME"
FINAL_WRAPPER_DIR="$REPO_ROOT/$WRAPPER_NAME"

declare -a ROOT_ENTRIES=()
declare -a FLUX_ENTRIES=()
RESUME_ONLY=0

if [[ -d "$FLUX_DIR" ]]; then
  if [[ -e "$TMP_ROOT" ]]; then
    echo "Temporary path already exists: $TMP_ROOT" >&2
    exit 1
  fi

  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    [[ "$entry" == ".git" ]] && continue
    [[ "$entry" == "$(basename "$TMP_ROOT")" ]] && continue
    ROOT_ENTRIES+=("$entry")
  done < <(find "$REPO_ROOT" -mindepth 1 -maxdepth 1 -printf '%f\n' | sort)

  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    FLUX_ENTRIES+=("$entry")
  done < <(find "$FLUX_DIR" -mindepth 1 -maxdepth 1 -printf '%f\n' | sort)
elif [[ -d "$WRAPPER_DIR" ]]; then
  RESUME_ONLY=1
else
  echo "Expected Flux source at $FLUX_DIR or a staged kernel tree at $WRAPPER_DIR" >&2
  exit 1
fi

if [[ -e "$FINAL_WRAPPER_DIR" && "$FINAL_WRAPPER_DIR" != "$WRAPPER_DIR" ]]; then
  echo "Final wrapper path already exists: $FINAL_WRAPPER_DIR" >&2
  exit 1
fi

run() {
  if (( APPLY )); then
    "$@"
  else
    printf '[dry-run] '
    printf '%q ' "$@"
    printf '\n'
  fi
}

echo "Repository root: $REPO_ROOT"
if (( RESUME_ONLY )); then
  echo "Flux source:     <already flattened>"
else
  echo "Flux source:     $FLUX_DIR"
fi
echo "Kernel staging:  $WRAPPER_DIR"
echo "Kernel final:    $FINAL_WRAPPER_DIR"
echo "Mode:            $([[ $APPLY -eq 1 ]] && echo apply || echo dry-run)"
echo
if (( RESUME_ONLY )); then
  echo "Detected partially completed restructure; only the final move will run."
else
  echo "Root entries that will move under $WRAPPER_NAME/:"
  printf '  %s\n' "${ROOT_ENTRIES[@]}"
  echo
  echo "Entries that will be flattened from tools/flux back to root:"
  printf '  %s\n' "${FLUX_ENTRIES[@]}"
  echo

  run mkdir -p "$WRAPPER_DIR"

  for entry in "${ROOT_ENTRIES[@]}"; do
    run mv "$REPO_ROOT/$entry" "$WRAPPER_DIR/"
  done

  for entry in "${FLUX_ENTRIES[@]}"; do
    run mv "$WRAPPER_DIR/tools/flux/$entry" "$REPO_ROOT/"
  done

  run rmdir "$WRAPPER_DIR/tools/flux"
  run rmdir "$WRAPPER_DIR/tools" || true
fi

run mv "$WRAPPER_DIR" "$FINAL_WRAPPER_DIR"
run rmdir "$TMP_ROOT"

if (( APPLY )); then
  echo
  echo "Restructure complete."
else
  echo "Dry-run complete. Re-run with --apply to execute."
fi
