#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/tick.h>
#include <asm/host_ops.h>
#include <asm/ptrace.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>

void read_persistent_clock64(struct timespec64 *ts)
{
	long ret;

	ret = host_syscall(__NR_clock_gettime, CLOCK_REALTIME, ts);
	if (ret < 0) {
		ts->tv_sec = 0;
		ts->tv_nsec = 0;
	}
}

#ifdef CONFIG_X86_64
unsigned long profile_pc(struct pt_regs *regs)
{
	unsigned long pc = instruction_pointer(regs);

	if (!user_mode(regs) && in_lock_functions(pc)) {
#ifdef CONFIG_FRAME_POINTER
		return *(unsigned long *)(regs->bp + sizeof(long));
#else
		unsigned long *sp = (unsigned long *)regs->sp;
		/*
		 * Return address is either directly at stack pointer
		 * or above a saved flags. Eflags has bits 22-31 zero,
		 * kernel addresses don't.
		 */
		if (sp[0] >> 22)
			return sp[0];
		if (sp[1] >> 22)
			return sp[1];
#endif
	}
	return pc;
}
#else
#error "Unsupported architecture"
#endif

#ifndef CONFIG_X86_TSC

static u64 native_clock(void)
{
	return flux_ops_time_raw();
}

static u64 clock_read(struct clocksource *cs)
{
	return native_clock();
}

static struct clocksource flux_clocksource = {
	.name = "flux",
	.rating = 499,
	.mask = CLOCKSOURCE_MASK(64),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
	.read = clock_read,
};

static unsigned long long boot_time;

void __ndelay(unsigned long nsecs)
{
	unsigned long long start = native_clock();

	while (native_clock() < start + nsecs)
		;
}

void __udelay(unsigned long usecs)
{
	__ndelay(usecs * NSEC_PER_USEC);
}

void __const_udelay(unsigned long xloops)
{
	__udelay(xloops / 0x10c7ul);
}

void calibrate_delay(void)
{
}

unsigned long long sched_clock(void)
{
	if (!boot_time)
		return 0;

	return native_clock() - boot_time;
}

void __init time_init(void)
{
	int ret;

	boot_time = native_clock();

	ret = clocksource_register_hz(&flux_clocksource, NSEC_PER_SEC);
	if (ret) {
		flux_debug("failed to register clocksource %d\n", ret);
		return;
	}

#ifdef CONFIG_FLUX_UINTR
	uintr_timer_init();
#else
	default_timer_init();
#endif

	return;
}
#endif /* !CONFIG_X86_TSC */