/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_FLUX_SEMBUF_H
#define _ASM_FLUX_SEMBUF_H

#include <asm/ipcbuf.h>

/*
 * Flux exposes the x86_64 userspace ABI, so SysV semaphore IPC structures
 * must match x86_64 rather than asm-generic. x86_64 has historical padding
 * after the time fields; without it userspace reads sem_nsems at the wrong
 * offset.
 */
struct semid64_ds {
	struct ipc64_perm sem_perm;
	__kernel_long_t sem_otime;
	__kernel_ulong_t __unused1;
	__kernel_long_t sem_ctime;
	__kernel_ulong_t __unused2;
	__kernel_ulong_t sem_nsems;
	__kernel_ulong_t __unused3;
	__kernel_ulong_t __unused4;
};

#endif /* _ASM_FLUX_SEMBUF_H */
