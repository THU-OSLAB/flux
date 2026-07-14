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
 * We use the full linear equation: f(x) = a + b*x, in order to allow
 * a continuous function in the face of dynamic freq changes.
 *
 * Continuity means that when our frequency changes our slope (b); we want to
 * ensure that: f(t) == f'(t), which gives: a + b*t == a' + b'*t.
 *
 * Without an offset (a) the above would not be possible.
 *
 * See the comment near cycles_2_ns() for details on how we compute (b).
 */
struct cyc2ns_data {
	u32 cyc2ns_mul;
	u32 cyc2ns_shift;
	u64 cyc2ns_offset;
}; /* 16 bytes */

struct cyc2ns {
	struct cyc2ns_data data[2]; /*  0 + 2*16 = 32 */
	seqcount_latch_t seq; /* 32 + 4 = 36 */
}; /* fits one cacheline */
static DEFINE_PER_CPU_ALIGNED(struct cyc2ns, cyc2ns);

static void delay_loop(u64 __loops);

/*
 * Calibration and selection of the delay mechanism happens only once
 * during boot.
 */
static void (*delay_fn)(u64) __ro_after_init = delay_loop;
static void (*delay_halt_fn)(u64 start, u64 cycles) __ro_after_init;

__always_inline void __cyc2ns_read(struct cyc2ns_data *data)
{
	int seq, idx;

	do {
		seq = this_cpu_read(cyc2ns.seq.seqcount.sequence);
		idx = seq & 1;

		data->cyc2ns_offset =
			this_cpu_read(cyc2ns.data[idx].cyc2ns_offset);
		data->cyc2ns_mul = this_cpu_read(cyc2ns.data[idx].cyc2ns_mul);
		data->cyc2ns_shift =
			this_cpu_read(cyc2ns.data[idx].cyc2ns_shift);
	} while (unlikely(seq != this_cpu_read(cyc2ns.seq.seqcount.sequence)));
}

__always_inline void cyc2ns_read_begin(struct cyc2ns_data *data)
{
	preempt_disable_notrace();
	__cyc2ns_read(data);
}

__always_inline void cyc2ns_read_end(void)
{
	preempt_enable_notrace();
}

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
	struct cyc2ns_data data;
	unsigned long long ns;

	__cyc2ns_read(&data);

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

static void __set_cyc2ns_scale(unsigned long khz, int cpu,
			       unsigned long long tsc_now)
{
	unsigned long long ns_now;
	struct cyc2ns_data data;
	struct cyc2ns *c2n;

	ns_now = cycles_2_ns(tsc_now);

	/*
	 * Compute a new multiplier as per the above comment and ensure our
	 * time function is continuous; see the comment near struct
	 * cyc2ns_data.
	 */
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

	c2n = per_cpu_ptr(&cyc2ns, cpu);

	raw_write_seqcount_latch(&c2n->seq);
	c2n->data[0] = data;
	raw_write_seqcount_latch(&c2n->seq);
	c2n->data[1] = data;
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

u64 sched_clock_noinstr(void) __attribute__((alias("native_sched_clock")));

notrace u64 sched_clock(void)
{
	u64 now;
	preempt_disable_notrace();
	now = sched_clock_noinstr();
	preempt_enable_notrace();
	return now;
}

/*
 * Initialize cyc2ns for boot cpu
 */
static void __init cyc2ns_init_boot_cpu(void)
{
	struct cyc2ns *c2n = this_cpu_ptr(&cyc2ns);

	seqcount_latch_init(&c2n->seq);
	__set_cyc2ns_scale(tsc_khz, smp_processor_id(), rdtsc());
}

/*
 * Secondary CPUs do not run through tsc_init(), so set up
 * all the scale factors for all CPUs, assuming the same
 * speed as the bootup CPU.
 */
static void __init cyc2ns_init_secondary_cpus(void)
{
	unsigned int cpu, this_cpu = smp_processor_id();
	struct cyc2ns *c2n = this_cpu_ptr(&cyc2ns);
	struct cyc2ns_data *data = c2n->data;

	for_each_possible_cpu(cpu) {
		if (cpu != this_cpu) {
			seqcount_latch_init(&c2n->seq);
			c2n = per_cpu_ptr(&cyc2ns, cpu);
			c2n->data[0] = data[0];
			c2n->data[1] = data[1];
		}
	}
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
	cyc2ns_init_boot_cpu();
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
	cyc2ns_init_secondary_cpus();
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

#ifdef CONFIG_FLUX_UINTR
	uintr_timer_init();
#else
	default_timer_init();
#endif

	return;
}