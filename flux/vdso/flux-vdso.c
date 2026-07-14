/* SPDX-License-Identifier: GPL-2.0 */

#include <asm/vdso.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define FLUX_ALWAYS_INLINE __inline__ __attribute__((always_inline))

#define FLUX_SYSCALL_GETTIMEOFDAY 96
#define FLUX_SYSCALL_CLOCK_GETTIME 228
#define FLUX_SYSCALL_CLOCK_GETRES 229

#define FLUX_CLOCK_REALTIME 0
#define FLUX_CLOCK_MONOTONIC 1
#define FLUX_CLOCK_MONOTONIC_RAW 4
#define FLUX_CLOCK_REALTIME_COARSE 5
#define FLUX_CLOCK_MONOTONIC_COARSE 6
#define FLUX_CLOCK_BOOTTIME 7
#define FLUX_CLOCK_TAI 11

#define FLUX_NSEC_PER_SEC 1000000000ULL
#define FLUX_NSEC_PER_USEC 1000ULL
#define FLUX_S64_MAX 0x7fffffffffffffffULL

struct flux_timespec {
	long tv_sec;
	long tv_nsec;
};

struct flux_timeval {
	long tv_sec;
	long tv_usec;
};

struct flux_timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

extern struct flux_vdso_data _vdso_data[FLUX_VDSO_DATA_COUNT]
	__attribute__((visibility("hidden")));
extern long __flux_vdso_syscall2(long nr, long arg1, long arg2)
	__attribute__((visibility("hidden")));

_Static_assert(sizeof(long) == sizeof(flux_vdso_u64),
	       "Flux vDSO requires x86-64");
_Static_assert(sizeof(struct flux_vdso_data) == 240,
	       "Flux vvar ABI changed unexpectedly");
_Static_assert(CONFIG_HZ > 0, "Flux vDSO requires a configured timer rate");

static __inline__ void flux_cpu_relax(void)
{
	__asm__ __volatile__("pause" ::: "memory");
}

static __inline__ flux_vdso_u32
flux_read_seq(const struct flux_vdso_data *vd)
{
	return __atomic_load_n(&vd->seq, __ATOMIC_RELAXED);
}

static __inline__ flux_vdso_u32
flux_read_begin(const struct flux_vdso_data *vd)
{
	flux_vdso_u32 seq;

	while (unlikely((seq = flux_read_seq(vd)) & 1))
		flux_cpu_relax();
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return seq;
}

static __inline__ int flux_read_retry(const struct flux_vdso_data *vd,
				      flux_vdso_u32 seq)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return unlikely(flux_read_seq(vd) != seq);
}

static __inline__ flux_vdso_u64 flux_rdtsc_ordered(void)
{
	flux_vdso_u32 low, high;

	__asm__ __volatile__("lfence\n\trdtsc"
			     : "=a"(low), "=d"(high)
			     :
			     : "memory");
	return ((flux_vdso_u64)high << 32) | low;
}

static __inline__ flux_vdso_u64
flux_delta_ns(flux_vdso_u64 cycles, flux_vdso_u64 last,
	      flux_vdso_u32 mult)
{
	flux_vdso_u64 delta = (cycles - last) & FLUX_S64_MAX;

	/* Match x86's protection against a slightly backward cross-CPU TSC. */
	if (unlikely(delta & (1ULL << 62)))
		return 0;
	return delta * mult;
}

static FLUX_ALWAYS_INLINE int
flux_hres(const struct flux_vdso_data *vd, int clock,
	  struct flux_timespec *ts)
{
	const struct flux_vdso_timestamp *base = &vd->basetime[clock];
	flux_vdso_u64 cycles, ns, sec;
	flux_vdso_u32 seq;

	do {
		seq = flux_read_begin(vd);
		if (unlikely(vd->clock_mode != FLUX_VDSO_CLOCKMODE_TSC))
			return -1;

		cycles = flux_rdtsc_ordered() & FLUX_S64_MAX;
		ns = base->nsec;
		ns += flux_delta_ns(cycles, vd->cycle_last, vd->mult);
		ns >>= vd->shift;
		sec = base->sec;
	} while (flux_read_retry(vd, seq));

	sec += ns / FLUX_NSEC_PER_SEC;
	ns %= FLUX_NSEC_PER_SEC;
	ts->tv_sec = (long)sec;
	ts->tv_nsec = (long)ns;
	return 0;
}

static FLUX_ALWAYS_INLINE int
flux_coarse(const struct flux_vdso_data *vd, int clock,
	    struct flux_timespec *ts)
{
	const struct flux_vdso_timestamp *base = &vd->basetime[clock];
	flux_vdso_u32 seq;

	do {
		seq = flux_read_begin(vd);
		ts->tv_sec = (long)base->sec;
		ts->tv_nsec = (long)base->nsec;
	} while (flux_read_retry(vd, seq));
	return 0;
}

static int flux_clock_gettime_fast(int clock, struct flux_timespec *ts)
{
	switch (clock) {
	case FLUX_CLOCK_REALTIME:
	case FLUX_CLOCK_MONOTONIC:
	case FLUX_CLOCK_BOOTTIME:
	case FLUX_CLOCK_TAI:
		return flux_hres(&_vdso_data[FLUX_VDSO_HRES_COARSE], clock,
				  ts);
	case FLUX_CLOCK_MONOTONIC_RAW:
		return flux_hres(&_vdso_data[FLUX_VDSO_RAW], clock, ts);
	case FLUX_CLOCK_REALTIME_COARSE:
	case FLUX_CLOCK_MONOTONIC_COARSE:
		return flux_coarse(&_vdso_data[FLUX_VDSO_HRES_COARSE], clock,
				    ts);
	default:
		return -1;
	}
}

int __vdso_clock_gettime(int clock, struct flux_timespec *ts)
{
	if (likely(flux_clock_gettime_fast(clock, ts) == 0))
		return 0;
	return (int)__flux_vdso_syscall2(FLUX_SYSCALL_CLOCK_GETTIME, clock,
					 (long)ts);
}

int clock_gettime(int clock, struct flux_timespec *ts)
	__attribute__((weak, alias("__vdso_clock_gettime")));

int __vdso_gettimeofday(struct flux_timeval *tv, struct flux_timezone *tz)
{
	const struct flux_vdso_data *vd =
		&_vdso_data[FLUX_VDSO_HRES_COARSE];
	struct flux_timespec ts;

	if (likely(tv != 0)) {
		if (unlikely(flux_hres(vd, FLUX_CLOCK_REALTIME, &ts) != 0))
			return (int)__flux_vdso_syscall2(
				FLUX_SYSCALL_GETTIMEOFDAY, (long)tv, (long)tz);
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / FLUX_NSEC_PER_USEC;
	}

	if (unlikely(tz != 0)) {
		tz->tz_minuteswest = vd->tz_minuteswest;
		tz->tz_dsttime = vd->tz_dsttime;
	}
	return 0;
}

int gettimeofday(struct flux_timeval *tv, struct flux_timezone *tz)
	__attribute__((weak, alias("__vdso_gettimeofday")));

long __vdso_time(long *result)
{
	long now = (long)__atomic_load_n(
		&_vdso_data[FLUX_VDSO_HRES_COARSE]
			 .basetime[FLUX_CLOCK_REALTIME]
			 .sec,
		__ATOMIC_RELAXED);

	if (result)
		*result = now;
	return now;
}

long time(long *result) __attribute__((weak, alias("__vdso_time")));

int __vdso_clock_getres(int clock, struct flux_timespec *res)
{
	flux_vdso_u64 nsec;

	switch (clock) {
	case FLUX_CLOCK_REALTIME:
	case FLUX_CLOCK_MONOTONIC:
	case FLUX_CLOCK_MONOTONIC_RAW:
	case FLUX_CLOCK_BOOTTIME:
	case FLUX_CLOCK_TAI:
		nsec = __atomic_load_n(
			&_vdso_data[FLUX_VDSO_HRES_COARSE].hrtimer_res,
			__ATOMIC_RELAXED);
#ifdef CONFIG_HIGH_RES_TIMERS
		/* hrtimer can switch to high resolution before the next update. */
		if (unlikely(nsec != 1))
			return (int)__flux_vdso_syscall2(
				FLUX_SYSCALL_CLOCK_GETRES, clock, (long)res);
#endif
		break;
	case FLUX_CLOCK_REALTIME_COARSE:
	case FLUX_CLOCK_MONOTONIC_COARSE:
		nsec = (FLUX_NSEC_PER_SEC + CONFIG_HZ / 2) / CONFIG_HZ;
		break;
	default:
		return (int)__flux_vdso_syscall2(FLUX_SYSCALL_CLOCK_GETRES,
						 clock, (long)res);
	}

	if (res) {
		res->tv_sec = 0;
		res->tv_nsec = (long)nsec;
	}
	return 0;
}

int clock_getres(int clock, struct flux_timespec *res)
	__attribute__((weak, alias("__vdso_clock_getres")));
