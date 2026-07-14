/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_FLUX_MPK_H
#define _UAPI_ASM_FLUX_MPK_H

#define FLUX_MPK_KERNEL_PKEY 0
#define FLUX_MPK_APP_PKEY 1
#define FLUX_MPK_SHARED_PKEY 2

#define FLUX_MPK_KERNEL_PKRU 0

/*
 * key 0: no access, key 1: read/write, key 2: read-only.
 * Unused keys are disabled as well.
 */
#define FLUX_MPK_APP_PKRU 0xffffffe3

#endif /* _UAPI_ASM_FLUX_MPK_H */
