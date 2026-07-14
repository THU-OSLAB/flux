#ifndef _FLUX_BASE_H
#define _FLUX_BASE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Preserve the current UAPI/libc coordination behavior for code that includes
 * the umbrella Flux userspace header.
 */
#define _FLUX_LIBC_COMPAT_H

#ifdef __cplusplus
#define class __flux__class
#endif

#include <kernel/asm/flux_ops.h>
#include <kernel/asm/host_ops.h>

/*
 * Avoid collisions between Android, which defines __unused, and
 * linux/icmp.h, which uses __unused as a structure field.
 */
#pragma push_macro("__unused")
#undef __unused

#include <kernel/asm/syscalls.h>

#pragma pop_macro("__unused")

#ifdef __cplusplus
#undef class
#endif

#if defined(__MINGW32__)
#define strtok_r strtok_s
#define inet_pton flux_inet_pton

int inet_pton(int af, const char *src, void *dst);
#endif

#ifdef __cplusplus
}
#endif

#endif
