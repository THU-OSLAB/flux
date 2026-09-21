/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_FLUX_VDSO_GETTIMEOFDAY_H
#define __ASM_FLUX_VDSO_GETTIMEOFDAY_H

#ifndef __ASSEMBLY__
#include <asm/barrier.h>
#include <asm/unistd.h>
#include <vdso/datapage.h>

#define VDSO_HAS_TIME 1
#define VDSO_HAS_CLOCK_GETRES 1

extern long __flux_vdso_syscall2(long nr, long arg1, long arg2)
	__attribute__((visibility("hidden")));

static __always_inline long
gettimeofday_fallback(struct __kernel_old_timeval *tv, struct timezone *tz)
{
	return __flux_vdso_syscall2(__NR_gettimeofday, (long)tv, (long)tz);
}

static __always_inline long
clock_gettime_fallback(clockid_t clock, struct __kernel_timespec *ts)
{
	return __flux_vdso_syscall2(__NR_clock_gettime, clock, (long)ts);
}

static __always_inline long
clock_getres_fallback(clockid_t clock, struct __kernel_timespec *ts)
{
	return __flux_vdso_syscall2(__NR_clock_getres, clock, (long)ts);
}

static __always_inline u64 __arch_get_hw_counter(s32 clock_mode,
						 const struct vdso_data *vd)
{
	u32 low, high;

	if (clock_mode != VDSO_CLOCKMODE_TSC)
		return U64_MAX;
	asm volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
	return (((u64)high << 32) | low) & S64_MAX;
}

static __always_inline bool vdso_cycles_ok(u64 cycles)
{
	return (s64)cycles >= 0;
}
#define vdso_cycles_ok vdso_cycles_ok

/* Match Linux x86: bit 63 marks invalid cycles; bit 62 catches TSC wobble. */
static __always_inline u64
vdso_calc_delta(u64 cycles, u64 last, u64 mask, u32 mult)
{
	u64 delta = (cycles - last) & S64_MAX;

	return unlikely(delta & (1ULL << 62)) ? 0 : delta * mult;
}
#define vdso_calc_delta vdso_calc_delta

static __always_inline const struct vdso_data *__arch_get_vdso_data(void)
{
	return _vdso_data;
}

#ifdef CONFIG_TIME_NS
static __always_inline const struct vdso_data *
__arch_get_timens_vdso_data(const struct vdso_data *vd)
{
	return _timens_data;
}
#endif
#endif
#endif
