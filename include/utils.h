#ifndef _UTILS_H
#define _UTILS_H

/*
 * Umbrella header for generic runtime utilities.
 *
 * Include narrower <utils/...> headers from internal headers when possible,
 * and use <utils.h> from C files that need the generic helper surface.
 *
 * Flux syscall-facing wrappers such as filesystem and sysctl helpers are
 * layered on top of this header and exported through <flux.h>.
 */
#include <utils/base.h>
#include <utils/error.h>
#include <utils/log.h>
#include <utils/lrpc.h>
#include <utils/memory.h>
#include <utils/path.h>
#include <utils/time.h>

#endif /* _UTILS_H */
