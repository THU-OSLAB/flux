// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

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
#include <asm/x86/uintr.h>
#include <asm/x86/irqflags.h>

int uintr_timer_irq;

struct uintr_timer {
	int cpu;
	void *handle;
	char name[64];
	struct clock_event_device *ce;
};

static int uintr_clock_next_event(unsigned long delta,
				  struct clock_event_device *ce)
{
	struct uintr_timer *timer =
		container_of((char (*)[64])ce->name, struct uintr_timer, name);

	flux_ops_timer_set_oneshot(timer->handle, delta);

	return 0;
}

static void uintr_clock_broadcast(const struct cpumask *mask)
{
#ifdef CONFIG_SMP
	int cpu;

	for_each_cpu(cpu, mask) {
		flux_tick_broadcast(cpu);
	}
#endif
}

static DEFINE_PER_CPU(struct uintr_timer, uintr_timer);

static DEFINE_PER_CPU(struct clock_event_device, uintr_clock_event) = {
	.name = "uintr_timer",
	.features = CLOCK_EVT_FEAT_ONESHOT,
	.rating = 499,
	.set_next_event = uintr_clock_next_event,
	.broadcast = uintr_clock_broadcast,
};

static int uintr_timer_starting_cpu(unsigned int cpu)
{
	struct clock_event_device *ce = per_cpu_ptr(&uintr_clock_event, cpu);
	struct uintr_timer *timer = per_cpu_ptr(&uintr_timer, cpu);
	struct flux_timer_args args;

	snprintf(timer->name, sizeof(timer->name), "uintr_timer%d", cpu);
	timer->ce = ce;
	timer->cpu = cpu;
	args.cpu = cpu;
	args.oneshot = 1; /* support periodic ? */
	timer->handle = flux_ops_timer_alloc(&args);
	if (!timer->handle) {
		flux_debug("failed to allocate uintr timer\n");
		return -ENODEV;
	}

	/* hack: ce->name implies the address of uintr_timer! */
	ce->name = timer->name;
	ce->cpumask = cpumask_of(cpu);
	ce->irq = uintr_timer_irq;

	clockevents_config_and_register(ce, NSEC_PER_SEC, 50000, 0x7fffffff);

	enable_percpu_irq(uintr_timer_irq,
			  irq_get_trigger_type(uintr_timer_irq));
	return 0;
}

static int uintr_timer_dying_cpu(unsigned int cpu)
{
	disable_percpu_irq(uintr_timer_irq);
	return 0;
}

static irqreturn_t uintr_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *dev = this_cpu_ptr(&uintr_clock_event);

	dev->event_handler(dev);

	return IRQ_HANDLED;
}

int __init uintr_timer_init(void)
{
	int ret = 0;

	uintr_timer_irq = irq_alloc_desc(NUMA_NO_NODE);
	if (uintr_timer_irq < 0) {
		flux_debug("failed to allocate uintr timer irq %d\n",
			  uintr_timer_irq);
		goto out;
	}
	irq_set_percpu_devid(uintr_timer_irq);
	irq_set_chip_and_handler(uintr_timer_irq, &dummy_irq_chip,
				 handle_percpu_irq);

	ret = request_percpu_irq(uintr_timer_irq, uintr_timer_interrupt,
				 "unitr-timer", &uintr_clock_event);
	if (ret) {
		flux_debug("failed to register uintr timer irq %d\n", ret);
		goto out;
	}

	ret = cpuhp_setup_state(CPUHP_AP_CLINT_TIMER_STARTING,
				"clockevents/flux/timer:starting",
				uintr_timer_starting_cpu,
				uintr_timer_dying_cpu);
	if (ret) {
		flux_debug("cpuhp setup state failed %d\n", ret);
		goto out_free_irq;
	}

	pr_info("timer initialized\n");

	return 0;
out_free_irq:
	free_percpu_irq(uintr_timer_irq, &uintr_clock_event);
out:
	flux_debug("failed to init timer\n");
	return ret;
}

void flux_cpu_clock_init(int cpu)
{
}
