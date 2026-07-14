#ifndef _FLUX_H
#define _FLUX_H

/*
 * Public umbrella header for the Flux userspace runtime.
 *
 * The API surface is split into smaller headers by responsibility, but
 * existing code can continue to include <flux.h> or "flux.h". Kernel/UAPI
 * compatibility headers are intentionally reached through <kernel/...>.
 */
#include <flux/base.h>
#include <flux/launch.h>
#include <flux/net.h>
#include <flux/runtime.h>
#include <flux/syscall.h>
#include <utils.h>
#include <utils/fs.h>
#include <utils/sysctl.h>

#endif
