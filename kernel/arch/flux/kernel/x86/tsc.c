// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <asm/x86/cpuid.h>
#include <asm/x86/cpufeature.h>
#include <asm/x86/cpufeatures.h>
#include <asm/x86/tsc.h>

unsigned int __read_mostly cpu_khz; /* TSC clocks / usec, not used here */
EXPORT_SYMBOL(cpu_khz);

unsigned int __read_mostly tsc_khz;
EXPORT_SYMBOL(tsc_khz);

#define KHZ 1000

/*
 * TSC can be unstable due to cpufreq or due to unsynced TSCs
 */
static unsigned int __initdata tsc_early_khz;

static DEFINE_STATIC_KEY_FALSE(__use_tsc);

/*
 * Flux calibrates once before enabling the TSC clock and gives every vCPU
 * the same conversion. There is no runtime frequency update or per-CPU
 * offset adjustment, so readers need neither a latch nor a CPU lookup.
 */
struct cyc2ns_data {
	u32 cyc2ns_mul;
	u32 cyc2ns_shift;
	u64 cyc2ns_offset;
};
static struct cyc2ns_data flux_cyc2ns __ro_after_init;

static void delay_loop(u64 __loops);

/*
 * Calibration and selection of the delay mechanism happens only once
 * during boot.
 */
static void (*delay_fn)(u64) __ro_after_init = delay_loop;
static void (*delay_halt_fn)(u64 start, u64 cycles) __ro_after_init;

/*
 * Accelerators for sched_clock()
 * convert from cycles(64bits) => nanoseconds (64bits)
 *  basic equation:
 *              ns = cycles / (freq / ns_per_sec)
 *              ns = cycles * (ns_per_sec / freq)
 *              ns = cycles * (10^9 / (cpu_khz * 10^3))
 *              ns = cycles * (10^6 / cpu_khz)
 *
 *      Then we use scaling math (suggested by george@mvista.com) to get:
 *              ns = cycles * (10^6 * SC / cpu_khz) / SC
 *              ns = cycles * cyc2ns_scale / SC
 *
 *      And since SC is a constant power of two, we can convert the div
 *  into a shift. The larger SC is, the more accurate the conversion, but
 *  cyc2ns_scale needs to be a 32-bit value so that 32-bit multiplication
 *  (64-bit result) can be used.
 *
 *  We can use khz divisor instead of mhz to keep a better precision.
 *  (mathieu.desnoyers@polymtl.ca)
 *
 *                      -johnstul@us.ibm.com "math is hard, lets go shopping!"
 */

static __always_inline unsigned long long __cycles_2_ns(unsigned long long cyc)
{
	struct cyc2ns_data data = flux_cyc2ns;
	unsigned long long ns;

	ns = data.cyc2ns_offset;
	ns += mul_u64_u32_shr(cyc, data.cyc2ns_mul, data.cyc2ns_shift);

	return ns;
}

static __always_inline unsigned long long cycles_2_ns(unsigned long long cyc)
{
	unsigned long long ns;
	preempt_disable_notrace();
	ns = __cycles_2_ns(cyc);
	preempt_enable_notrace();
	return ns;
}

static void __init flux_init_cyc2ns(unsigned long khz,
			       unsigned long long tsc_now)
{
	unsigned long long ns_now;
	struct cyc2ns_data data;

	ns_now = cycles_2_ns(tsc_now);

	/* Preserve continuity with the pre-calibration (zero) clock. */
	clocks_calc_mult_shift(&data.cyc2ns_mul, &data.cyc2ns_shift, khz,
			       NSEC_PER_MSEC, 0);

	/*
	 * cyc2ns_shift is exported via arch_perf_update_userpage() where it is
	 * not expected to be greater than 31 due to the original published
	 * conversion algorithm shifting a 32-bit value (now specifies a 64-bit
	 * value) - refer perf_event_mmap_page documentation in perf_event.h.
	 */
	if (data.cyc2ns_shift == 32) {
		data.cyc2ns_shift = 31;
		data.cyc2ns_mul >>= 1;
	}

	data.cyc2ns_offset = ns_now - mul_u64_u32_shr(tsc_now, data.cyc2ns_mul,
						      data.cyc2ns_shift);

	flux_cyc2ns = data;
}

/*
 * Scheduler clock - returns current time in nanosec units.
 */
noinstr u64 native_sched_clock(void)
{
	if (static_branch_likely(&__use_tsc)) {
		u64 tsc_now = rdtsc();

		/* return the value in ns */
		return __cycles_2_ns(tsc_now);
	}

	/*
	 * Fall back to jiffies if there's no TSC available:
	 * ( But note that we still use it if the TSC is marked
	 *   unstable. We do this because unlike Time Of Day,
	 *   the scheduler clock tolerates small errors and it's
	 *   very important for it to be as fast as the platform
	 *   can achieve it. )
	 */

	/* No locking but a rare wrong value is not a big deal: */
	return (jiffies_64 - INITIAL_JIFFIES) * (1000000000 / HZ);
}

/*
 * Generate a sched_clock if you already have a TSC value.
 */
u64 native_sched_clock_from_tsc(u64 tsc)
{
	return cycles_2_ns(tsc);
}

#if defined(CONFIG_ARCH_WANTS_NO_INSTR) || defined(CONFIG_GENERIC_SCHED_CLOCK)
u64 sched_clock_noinstr(void) __attribute__((alias("native_sched_clock")));
#define flux_sched_clock_noinstr sched_clock_noinstr
#else
#define flux_sched_clock_noinstr native_sched_clock
#endif

notrace u64 sched_clock(void)
{
	u64 now;
	preempt_disable_notrace();
	now = flux_sched_clock_noinstr();
	preempt_enable_notrace();
	return now;
}

static unsigned long __init get_loops_per_jiffy(void)
{
	u64 lpj = (u64)tsc_khz * KHZ;

	do_div(lpj, HZ);
	return lpj;
}

static void __init tsc_enable_sched_clock(void)
{
	loops_per_jiffy = get_loops_per_jiffy();
	flux_init_cyc2ns(tsc_khz, rdtsc());
	static_branch_enable(&__use_tsc);
}

static unsigned long cpu_khz_from_cpuid(void)
{
	unsigned int eax_base_mhz, ebx_max_mhz, ecx_bus_mhz, edx;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_INTEL)
		return 0;

	if (boot_cpu_data.cpuid_level < 0x16)
		return 0;

	eax_base_mhz = ebx_max_mhz = ecx_bus_mhz = edx = 0;

	cpuid(0x16, &eax_base_mhz, &ebx_max_mhz, &ecx_bus_mhz, &edx);

	return eax_base_mhz * 1000;
}

/*
 * Determine TSC frequency via CPUID, else return 0.
 */
unsigned long tsc_khz_from_cpuid(void)
{
	unsigned int eax_denominator, ebx_numerator, ecx_hz, edx;
	unsigned int crystal_khz;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_INTEL)
		return 0;

	if (boot_cpu_data.cpuid_level < 0x15)
		return 0;

	eax_denominator = ebx_numerator = ecx_hz = edx = 0;

	/* CPUID 15H TSC/Crystal ratio, plus optionally Crystal Hz */
	cpuid(0x15, &eax_denominator, &ebx_numerator, &ecx_hz, &edx);

	if (ebx_numerator == 0 || eax_denominator == 0)
		return 0;

	crystal_khz = ecx_hz / 1000;

	/*
	 * Some Intel SoCs like Skylake and Kabylake don't report the crystal
	 * clock, but we can easily calculate it to a high degree of accuracy
	 * by considering the crystal ratio and the CPU speed.
	 */
	if (crystal_khz == 0 && boot_cpu_data.cpuid_level >= 0x16) {
		unsigned int eax_base_mhz, ebx, ecx, edx;

		cpuid(0x16, &eax_base_mhz, &ebx, &ecx, &edx);
		crystal_khz =
			eax_base_mhz * 1000 * eax_denominator / ebx_numerator;
	}

	if (crystal_khz == 0)
		return 0;

	return crystal_khz * ebx_numerator / eax_denominator;
}

static bool __init determine_cpu_tsc_frequencies(bool early)
{
	/* Make sure that cpu and tsc are not already calibrated */
	WARN_ON(cpu_khz || tsc_khz);

	if (early) {
		cpu_khz = cpu_khz_from_cpuid();
		if (tsc_early_khz)
			tsc_khz = tsc_early_khz;
		else
			tsc_khz = tsc_khz_from_cpuid();
	} else {
		pr_warn("Non-early TSC frequency detection not implemented\n");
	}

	/*
	 * Trust non-zero tsc_khz as authoritative,
	 * and use it to sanity check cpu_khz,
	 * which will be off if system timer is off.
	 */
	if (tsc_khz == 0)
		tsc_khz = cpu_khz;
	else if (abs(cpu_khz - tsc_khz) * 10 > tsc_khz)
		cpu_khz = tsc_khz;

	if (tsc_khz == 0)
		return false;

	pr_info("Detected %lu.%03lu MHz processor\n",
		(unsigned long)cpu_khz / KHZ, (unsigned long)cpu_khz % KHZ);

	if (cpu_khz != tsc_khz) {
		pr_info("Detected %lu.%03lu MHz TSC",
			(unsigned long)tsc_khz / KHZ,
			(unsigned long)tsc_khz % KHZ);
	}
	return true;
}

void __init tsc_init(void)
{
	if (!boot_cpu_has(X86_FEATURE_TSC))
		return;
	if (!determine_cpu_tsc_frequencies(true))
		return;
	tsc_enable_sched_clock();
	lpj_fine = get_loops_per_jiffy();
}

/* simple loop based delay: */
static void delay_loop(u64 __loops)
{
	unsigned long loops = (unsigned long)__loops;

	asm volatile("	test %0,%0	\n"
		     "	jz 3f		\n"
		     "	jmp 1f		\n"

		     ".align 16		\n"
		     "1:	jmp 2f		\n"

		     ".align 16		\n"
		     "2:	dec %0		\n"
		     "	jnz 2b		\n"
		     "3:	dec %0		\n"

		     : "+a"(loops)
		     :);
}

/*
 * On Intel the TPAUSE instruction waits until any of:
 * 1) the TSC counter exceeds the value provided in EDX:EAX
 * 2) global timeout in IA32_UMWAIT_CONTROL is exceeded
 * 3) an external interrupt occurs
 */
static void delay_halt_tpause(u64 start, u64 cycles)
{
	u64 until = start + cycles;
	u32 eax, edx;

	eax = lower_32_bits(until);
	edx = upper_32_bits(until);

	/*
	 * Hard code the deeper (C0.2) sleep state because exit latency is
	 * small compared to the "microseconds" that usleep() will delay.
	 */
	__tpause(TPAUSE_C02_STATE, edx, eax);
}

/*
 * Call a vendor specific function to delay for a given amount of time. Because
 * these functions may return earlier than requested, check for actual elapsed
 * time and call again until done.
 */
static void delay_halt(u64 __cycles)
{
	u64 start, end, cycles = __cycles;

	/*
	 * Timer value of 0 causes MWAITX to wait indefinitely, unless there
	 * is a store on the memory monitored by MONITORX.
	 */
	if (!cycles)
		return;

	start = rdtsc_ordered();

	for (;;) {
		delay_halt_fn(start, cycles);
		end = rdtsc_ordered();

		if (cycles <= end - start)
			break;

		cycles -= end - start;
		start = end;
	}
}

void __delay(unsigned long loops)
{
	delay_fn(loops);
}
EXPORT_SYMBOL(__delay);

noinline void __const_udelay(unsigned long xloops)
{
	unsigned long lpj = loops_per_jiffy;
	int d0;

	xloops *= 4;
	asm("mull %%edx"
	    : "=d"(xloops), "=&a"(d0)
	    : "1"(xloops), "0"(lpj * (HZ / 4)));

	__delay(++xloops);
}
EXPORT_SYMBOL(__const_udelay);

void __udelay(unsigned long usecs)
{
	__const_udelay(usecs * 0x000010c7); /* 2**32 / 1000000 (rounded up) */
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__const_udelay(nsecs * 0x00005); /* 2**32 / 1000000000 (rounded up) */
}
EXPORT_SYMBOL(__ndelay);

static void __init use_tpause_delay(void)
{
	delay_halt_fn = delay_halt_tpause;
	delay_fn = delay_halt;
}

/*
 * We used to compare the TSC to the cycle_last value in the clocksource
 * structure to avoid a nasty time-warp. This can be observed in a
 * very small window right after one CPU updated cycle_last under
 * xtime/vsyscall_gtod lock and the other CPU reads a TSC value which
 * is smaller than the cycle_last reference value due to a TSC which
 * is slightly behind. This delta is nowhere else observable, but in
 * that case it results in a forward time jump in the range of hours
 * due to the unsigned delta calculation of the time keeping core
 * code, which is necessary to support wrapping clocksources like pm
 * timer.
 *
 * This sanity check is now done in the core timekeeping code.
 * checking the result of read_tsc() - cycle_last for being negative.
 * That works because CLOCKSOURCE_MASK(64) does not mask out any bit.
 */
static u64 read_tsc(struct clocksource *cs)
{
	return (u64)rdtsc_ordered();
}

/*
 * Must mark VALID_FOR_HRES early such that when we unregister tsc_early
 * this one will immediately take over. We will only register if TSC has
 * been found good.
 */
static struct clocksource clocksource_tsc = {
	.name = "tsc",
	.rating = 300,
	.read = read_tsc,
	.mask = CLOCKSOURCE_MASK(64),
	.vdso_clock_mode = VDSO_CLOCKMODE_TSC,
	.flags = CLOCK_SOURCE_IS_CONTINUOUS | CLOCK_SOURCE_VALID_FOR_HRES |
		 CLOCK_SOURCE_MUST_VERIFY | CLOCK_SOURCE_VERIFY_PERCPU,
	.list = LIST_HEAD_INIT(clocksource_tsc.list),
};

void __init time_init(void)
{
	tsc_init();

	if (boot_cpu_has(X86_FEATURE_WAITPKG))
		use_tpause_delay();

	clocksource_register_khz(&clocksource_tsc, tsc_khz);

	uintr_timer_init();

	return;
}
