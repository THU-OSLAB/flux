/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_FLUX_VDSO_VSYSCALL_H
#define __ASM_FLUX_VDSO_VSYSCALL_H

#ifndef __ASSEMBLY__

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/timekeeper_internal.h>
#include <vdso/datapage.h>
#include <uapi/asm/vdso.h>

extern void *flux_vvar_data;

static_assert(VDSO_CLOCKMODE_TSC == FLUX_VDSO_CLOCKMODE_TSC);
static_assert(VDSO_BASES == FLUX_VDSO_BASES);
static_assert(CS_BASES == FLUX_VDSO_DATA_COUNT);
static_assert(sizeof(struct vdso_data) == sizeof(struct flux_vdso_data));
static_assert(offsetof(struct vdso_data, seq) ==
	      offsetof(struct flux_vdso_data, seq));
static_assert(offsetof(struct vdso_data, clock_mode) ==
	      offsetof(struct flux_vdso_data, clock_mode));
static_assert(offsetof(struct vdso_data, cycle_last) ==
	      offsetof(struct flux_vdso_data, cycle_last));
static_assert(offsetof(struct vdso_data, mask) ==
	      offsetof(struct flux_vdso_data, mask));
static_assert(offsetof(struct vdso_data, mult) ==
	      offsetof(struct flux_vdso_data, mult));
static_assert(offsetof(struct vdso_data, shift) ==
	      offsetof(struct flux_vdso_data, shift));
static_assert(offsetof(struct vdso_data, basetime) ==
	      offsetof(struct flux_vdso_data, basetime));
static_assert(offsetof(struct vdso_data, tz_minuteswest) ==
	      offsetof(struct flux_vdso_data, tz_minuteswest));
static_assert(offsetof(struct vdso_data, tz_dsttime) ==
	      offsetof(struct flux_vdso_data, tz_dsttime));
static_assert(offsetof(struct vdso_data, hrtimer_res) ==
	      offsetof(struct flux_vdso_data, hrtimer_res));

static __always_inline struct vdso_data *__flux_get_k_vdso_data(void)
{
	return (struct vdso_data *)flux_vvar_data;
}
#define __arch_get_k_vdso_data __flux_get_k_vdso_data

#include <asm-generic/vdso/vsyscall.h>

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_FLUX_VDSO_VSYSCALL_H */
