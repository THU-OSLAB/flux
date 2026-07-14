#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: build-host-kernel.sh [options]

Build a patched Ubuntu host kernel matching the current running kernel.

Options:
  --workdir DIR     Working directory for downloaded kernel sources.
                    Default: $HOME/work/kernel
  --install         Install the generated linux-image/linux-headers packages.
  --jobs N          Parallel build jobs. Default: nproc
  --localversion S  LOCALVERSION passed to the kernel build. Default: -flux
  -h, --help        Show this help.
EOF
}

log() {
  printf '[build-host-kernel] %s\n' "$*"
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

enable_deb_src() {
  local source_file

  shopt -s nullglob
  for source_file in /etc/apt/sources.list.d/*.sources; do
    if grep -q '^Types: deb$' "$source_file"; then
      log "enabling deb-src in $source_file"
      sudo sed -i 's/^Types: deb$/Types: deb deb-src/' "$source_file"
    fi
  done
  shopt -u nullglob
}

WORKDIR="${HOME}/work/kernel"
INSTALL_PACKAGES=0
JOBS="$(nproc)"
LOCALVERSION="-flux"

while (($#)); do
  case "$1" in
    --workdir)
      WORKDIR="$2"
      shift 2
      ;;
    --install)
      INSTALL_PACKAGES=1
      shift
      ;;
    --jobs)
      JOBS="$2"
      shift 2
      ;;
    --localversion)
      LOCALVERSION="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

require_cmd apt-cache
require_cmd apt
require_cmd dpkg
require_cmd make
require_cmd patch
require_cmd sed
require_cmd awk
require_cmd find
require_cmd sudo

KREL="$(uname -r)"
PATCH_FILE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/0001-fork-export-dup_mm.patch"

if [[ ! -f "$PATCH_FILE" ]]; then
  printf 'patch file not found: %s\n' "$PATCH_FILE" >&2
  exit 1
fi

SRC_PKG="$(apt-cache show "linux-image-unsigned-${KREL}" 2>/dev/null | sed -n 's/^Source: //p' | head -n1)"
SRC_VER="$(apt-cache show "linux-image-unsigned-${KREL}" 2>/dev/null | sed -n 's/^Version: //p' | head -n1)"

if [[ -z "$SRC_PKG" || -z "$SRC_VER" ]]; then
  printf 'failed to resolve source package for kernel %s\n' "$KREL" >&2
  exit 1
fi

log "kernel release: $KREL"
log "source package: ${SRC_PKG}=${SRC_VER}"

enable_deb_src

log "updating apt indices"
sudo apt update

log "installing build dependencies"
sudo apt install -y \
  bc \
  bison \
  build-essential \
  debhelper \
  fakeroot \
  flex \
  gcc-x86-64-linux-gnu \
  kmod \
  libdw-dev \
  libelf-dev \
  libssl-dev \
  python3 \
  rsync

mkdir -p "$WORKDIR"
cd "$WORKDIR"

if ! ls | grep -q '^linux-'; then
  log "downloading kernel source"
  apt source "${SRC_PKG}=${SRC_VER}"
fi

SRC_DIR="$(find . -maxdepth 1 -mindepth 1 -type d -name 'linux-*' | sort | head -n1)"
if [[ -z "$SRC_DIR" ]]; then
  printf 'failed to locate extracted kernel source under %s\n' "$WORKDIR" >&2
  exit 1
fi

cd "$SRC_DIR"
log "using source tree: $(pwd)"

if patch -p1 --dry-run --forward < "$PATCH_FILE" >/dev/null 2>&1; then
  log "applying dup_mm export patch"
  patch -p1 --forward < "$PATCH_FILE"
else
  log "patch already applied or tree does not match, skipping patch step"
fi

if grep -q 'debhelper-compat (= 12)' debian/control; then
  log "updating debhelper compatibility to 13 for Ubuntu 24.04"
  sed -i 's/debhelper-compat (= 12)/debhelper-compat (= 13)/' debian/control
fi

log "copying host kernel config"
cp "/boot/config-${KREL}" .config

log "disabling missing Canonical certificate references"
./scripts/config --set-str SYSTEM_TRUSTED_KEYS ''
./scripts/config --set-str SYSTEM_REVOCATION_KEYS ''

log "refreshing config"
make olddefconfig

log "building Debian packages"
make -j"${JOBS}" bindeb-pkg LOCALVERSION="${LOCALVERSION}"

cd ..

if ((INSTALL_PACKAGES)); then
  log "installing generated packages"
  sudo dpkg -i ./*"${LOCALVERSION}"*.deb
  log "installation complete; reboot into the new kernel before rebuilding kmod"
else
  log "build complete"
  log "to install: sudo dpkg -i $(pwd)/*${LOCALVERSION}*.deb"
fi

log "after reboot, verify with: uname -r && sudo grep ' dup_mm$' /proc/kallsyms"
