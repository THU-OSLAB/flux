#ifndef _ASM_X86_TSC_H
#define _ASM_X86_TSC_H

#include <linux/stddef.h>

/* Using 64-bit values saves one instruction clearing the high half of low */
#define DECLARE_ARGS(val, low, high) unsigned long low, high
#define EAX_EDX_VAL(val, low, high) ((low) | (high) << 32)
#define EAX_EDX_RET(val, low, high) "=a"(low), "=d"(high)

/**
 * rdtsc() - returns the current TSC without ordering constraints
 *
 * rdtsc() returns the result of RDTSC as a 64-bit integer.  The
 * only ordering constraint it supplies is the ordering implied by
 * "asm volatile": it will put the RDTSC in the place you expect.  The
 * CPU can and will speculatively execute that RDTSC, though, so the
 * results can be non-monotonic if compared on different CPUs.
 */
static __always_inline unsigned long long rdtsc(void)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("rdtsc" : EAX_EDX_RET(val, low, high));

	return EAX_EDX_VAL(val, low, high);
}

/**
 * rdtsc_ordered() - read the current TSC in program order
 *
 * rdtsc_ordered() returns the result of RDTSC as a 64-bit integer.
 * It is ordered like a load to a global in-memory counter.  It should
 * be impossible to observe non-monotonic rdtsc_unordered() behavior
 * across multiple CPUs as long as the TSC is synced.
 */
static __always_inline unsigned long long rdtsc_ordered(void)
{
	DECLARE_ARGS(val, low, high);

	/*
	 * The RDTSC instruction is not ordered relative to memory
	 * access.  The Intel SDM and the AMD APM are both vague on this
	 * point, but empirically an RDTSC instruction can be
	 * speculatively executed before prior loads.  An RDTSC
	 * immediately after an appropriate barrier appears to be
	 * ordered as a normal load, that is, it provides the same
	 * ordering guarantees as reading from a global memory location
	 * that some other imaginary CPU is updating continuously with a
	 * time stamp.
	 *
	 * Thus, use the preferred barrier on the respective CPU, aiming for
	 * RDTSCP as the default.
	 */
	asm volatile("rdtscp"
		     : EAX_EDX_RET(val, low, high)
		     /* RDTSCP clobbers ECX with MSR_TSC_AUX. */
		     ::"ecx");

	return EAX_EDX_VAL(val, low, high);
}

static inline void _umonitor(volatile u64 *addr)
{
	asm volatile(".byte 0xf3, 0x0f, 0xae, 0xf7" : : "D"(addr));
}

static inline void _umwait(const u64 timeout)
{
	const u32 tsc_l = (u32)timeout;
	const u32 tsc_h = (u32)(timeout >> 32);

	asm volatile(".byte 0xf2, 0x0f, 0xae, 0xf7"
		     : /* ignore rflags */
		     : "D"(0), /* enter C0.2 */
		       "a"(tsc_l), "d"(tsc_h));
}

#define TPAUSE_C01_STATE 1
#define TPAUSE_C02_STATE 0

/*
 * Caller can specify whether to enter C0.1 (low latency, less
 * power saving) or C0.2 state (saves more power, but longer wakeup
 * latency). This may be overridden by the IA32_UMWAIT_CONTROL MSR
 * which can force requests for C0.2 to be downgraded to C0.1.
 */
static inline void __tpause(u32 ecx, u32 edx, u32 eax)
{
/* "tpause %ecx, %edx, %eax;" */
#ifdef CONFIG_AS_TPAUSE
	asm volatile("tpause %%ecx\n" : : "c"(ecx), "d"(edx), "a"(eax));
#else
	asm volatile(".byte 0x66, 0x0f, 0xae, 0xf1\t\n"
		     :
		     : "c"(ecx), "d"(edx), "a"(eax));
#endif
}

extern void tsc_init(void);

#endif /* _ASM_X86_TSC_H */