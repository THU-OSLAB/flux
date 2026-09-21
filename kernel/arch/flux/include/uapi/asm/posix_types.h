/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_UAPI_FLUX_POSIX_TYPES_H
#define _ASM_UAPI_FLUX_POSIX_TYPES_H

/*
 * Match the x86-64 UAPI layout: historical structure fields retain the
 * 16-bit old uid/gid types, while the native uid/gid types are 32-bit.
 */
typedef unsigned short __kernel_old_uid_t;
typedef unsigned short __kernel_old_gid_t;
#define __kernel_old_uid_t __kernel_old_uid_t

typedef unsigned long __kernel_old_dev_t;
#define __kernel_old_dev_t __kernel_old_dev_t

#include <asm-generic/posix_types.h>

#endif /* _ASM_UAPI_FLUX_POSIX_TYPES_H */
