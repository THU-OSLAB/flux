# Flux kernel patch series

This directory contains the ordered generic Linux 6.6 integration patches
needed by Flux.  They are kept beside the Flux source so every consumer uses
the same series instead of maintaining a private copy in an LTP or container
workflow.

Patch files are applied in bytewise filename order.  Use the repository helper
from the Flux root:

```sh
# Verify that the complete series applies and restores cleanly.
scripts/flux-kernel-patches.sh check

# Build with the series active, then restore the source tree.
scripts/flux-kernel-patches.sh run -- \
  make -B KCONFIG=linux66_compat_defconfig RUNC=1 FNET=0 SPDK=0

# Leave the patches applied for development, then undo the exact series.
scripts/flux-kernel-patches.sh apply
scripts/flux-kernel-patches.sh reverse

# Record the ordered file names and SHA-256 digests.
scripts/flux-kernel-patches.sh manifest
```

`run` executes the command from the Flux repository root, preserves its exit
status, and reverses every applied patch on
normal exit, failure, or signal.  It also verifies that the tracked source
state after restoration matches the state before application.  Use a forced or
otherwise clean build while the patches are active: restoring patched source
files and then performing a plain incremental build can replace required
objects with unpatched ones.

The series includes `/proc`, overlayfs, Landlock, scheduler, seccomp, mmap,
fork, vmalloc, ELF, shmem/THP, vsock, and rewrite-mapping integration.  In
particular, `0010-fork-arch-dup-mmap-prepare-hook.patch` pairs
`arch_dup_mmap_prepare()` with `arch_dup_mmap_finish()` around `dup_mmap()`;
without those calls Flux `fork()` cannot establish its host execution-mm alias.
