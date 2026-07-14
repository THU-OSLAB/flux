/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_FLUX_VDSO_H
#define _UAPI_ASM_FLUX_VDSO_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 flux_vdso_u32;
typedef __s32 flux_vdso_s32;
typedef __u64 flux_vdso_u64;
#else
#include <stdint.h>
typedef uint32_t flux_vdso_u32;
typedef int32_t flux_vdso_s32;
typedef uint64_t flux_vdso_u64;
#endif

#define FLUX_VDSO_CLOCKMODE_NONE	0
#define FLUX_VDSO_CLOCKMODE_TSC		1

#define FLUX_VDSO_BASES		12
#define FLUX_VDSO_HRES_COARSE	0
#define FLUX_VDSO_RAW		1
#define FLUX_VDSO_DATA_COUNT	2

struct flux_vdso_timestamp {
	flux_vdso_u64 sec;
	flux_vdso_u64 nsec;
};

/* Stable layout shared by the Flux kernel and the embedded userspace vDSO. */
struct flux_vdso_data {
	flux_vdso_u32 seq;
	flux_vdso_s32 clock_mode;
	flux_vdso_u64 cycle_last;
	flux_vdso_u64 mask;
	flux_vdso_u32 mult;
	flux_vdso_u32 shift;
	struct flux_vdso_timestamp basetime[FLUX_VDSO_BASES];
	flux_vdso_s32 tz_minuteswest;
	flux_vdso_s32 tz_dsttime;
	flux_vdso_u32 hrtimer_res;
	flux_vdso_u32 unused;
};

#endif /* _UAPI_ASM_FLUX_VDSO_H */
