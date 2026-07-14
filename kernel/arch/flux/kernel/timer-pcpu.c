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

static int flux_timer_irq;

#define CLOCKEVENT_NAMELEN 64
struct flux_clock_event {
	void *timer;
	struct clock_event_device dev;
	char name[CLOCKEVENT_NAMELEN];
	struct flux_timer_args args;
};

static void timer_fn(int cpu)
{
	flux_set_remote_irq_pending(cpu, flux_timer_irq);
}

static int clock_event_set_next_event(unsigned long ns,
				      struct clock_event_device *evt)
{
	struct flux_clock_event *flux_ce =
		container_of(evt, struct flux_clock_event, dev);
	return flux_ops_timer_set_oneshot(flux_ce->timer, ns);
}

static void clock_event_broadcast(const struct cpumask *mask)
{
#ifdef CONFIG_SMP
	int cpu;

	for_each_cpu(cpu, mask) {
		flux_tick_broadcast(cpu);
	}
#endif
}

static DEFINE_PER_CPU_ALIGNED(struct flux_clock_event, pcpu_clock_event) = {
	.dev = {
		.features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_C3STOP,
		.rating = 399,
		.set_next_event = clock_event_set_next_event,
		.broadcast = clock_event_broadcast,
	},
};

static int flux_timer_starting_cpu(unsigned int cpu)
{
	struct flux_clock_event *flux_ce = per_cpu_ptr(&pcpu_clock_event, cpu);
	struct clock_event_device *ce = &flux_ce->dev;
	struct flux_timer_args *args = &flux_ce->args;

	snprintf(flux_ce->name, sizeof(flux_ce->name), "uintr_timer%d", cpu);
	args->fn = timer_fn;
	args->cpu = cpu;
	args->oneshot = 1;

	flux_ce->timer = flux_ops_timer_alloc(args);
	if (!flux_ce->timer) {
		flux_debug("failed to allocate timer\n");
		return -ENODEV;
	}

	ce->name = flux_ce->name;
	ce->cpumask = cpumask_of(cpu);
	ce->irq = flux_timer_irq;

	clockevents_config_and_register(ce, NSEC_PER_SEC, 100000, 0x7fffffff);

	enable_percpu_irq(flux_timer_irq, irq_get_trigger_type(flux_timer_irq));
	return 0;
}

static int flux_timer_dying_cpu(unsigned int cpu)
{
	struct flux_clock_event *flux_ce = per_cpu_ptr(&pcpu_clock_event, cpu);

	disable_percpu_irq(flux_timer_irq);

	if (flux_ce->timer) {
		flux_ops_timer_free(flux_ce->timer);
		flux_ce->timer = NULL;
	}

	return 0;
}

static irqreturn_t flux_timer_irq_handler(int irq, void *dev_id)
{
	struct clock_event_device *dev = &this_cpu_ptr(&pcpu_clock_event)->dev;

	dev->event_handler(dev);

	return IRQ_HANDLED;
}

int __init default_timer_init(void)
{
	int ret = 0;

	flux_timer_irq = irq_alloc_desc(NUMA_NO_NODE);
	if (flux_timer_irq < 0) {
		flux_debug("failed to allocate timer irq %d\n", flux_timer_irq);
		goto out;
	}
	irq_set_percpu_devid(flux_timer_irq);
	irq_set_chip_and_handler(flux_timer_irq, &dummy_irq_chip,
				 handle_percpu_irq);

	ret = request_percpu_irq(flux_timer_irq, flux_timer_irq_handler,
				 "flux-timer", &pcpu_clock_event);
	if (ret) {
		flux_debug("failed to register uintr timer irq %d\n", ret);
		goto out;
	}

	ret = cpuhp_setup_state(CPUHP_AP_CLINT_TIMER_STARTING,
				"clock_events/flux/timer:starting",
				flux_timer_starting_cpu, flux_timer_dying_cpu);
	if (ret) {
		flux_debug("cpuhp setup state failed %d\n", ret);
		goto out_free_irq;
	}

	return 0;
out_free_irq:
	free_percpu_irq(flux_timer_irq, &pcpu_clock_event);
out:
	flux_debug("failed to init timer\n");
	return ret;
}

void flux_cpu_clock_init(int cpu)
{
}
