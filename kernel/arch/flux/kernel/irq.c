#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/hardirq.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/tick.h>
#include <linux/bitmap.h>
#include <asm/irq_regs.h>
#include <asm/irqflags.h>
#include <asm/host_ops.h>

/*
 * To avoid much overhead we use an indirect approach: the irqs are marked using
 * a bitmap (array of longs) and a summary of the modified bits is kept in a
 * separate "index" long - one bit for each sizeof(long). Thus we can support
 * 4096 irqs on 64bit platforms and 1024 irqs on 32bit platforms.
 *
 * Whenever an irq is trigger both the array and the index is updated. To find
 * which irqs were triggered we first search the index and then the
 * corresponding part of the arrary.
 */
struct irq_status {
	unsigned long bits[NR_IRQS / BITS_PER_LONG];
	unsigned long index;
};

static struct irq_status *__irq_status;
#define current_irq_status \
	((struct irq_status *)&__irq_status[smp_processor_id()])

static DECLARE_BITMAP(irq_alloc, NR_IRQS) = { 0 };
static const char **irq_users;

#ifndef CONFIG_FLUX_UINTR

// It need to be aligned to cache line size, so we 
// need to add padding to fit its size as 64 bytes
struct IrqEnabled {
	unsigned long data;
	char padding[64 - sizeof(unsigned long)];
};

static struct IrqEnabled irq_enabled[NR_CPUS];

unsigned long arch_local_save_flags(void)
{
	// return per_cpu(irq_enabled, smp_processor_id());
	return irq_enabled[smp_processor_id()].data;
}

void arch_local_irq_restore(unsigned long flags)
{
	/* check pending irqs */
	if (flags == ARCH_IRQ_ENABLED &&
	    irq_enabled[smp_processor_id()].data == ARCH_IRQ_DISABLED &&
	    !in_interrupt()) {
		flux_run_irqs();
	}
	irq_enabled[smp_processor_id()].data = flags;
}

#endif

void init_IRQ(void)
{
	int i;
	struct irq_status *irq_status;

	__irq_status =
		kmalloc_array(NR_CPUS, sizeof(struct irq_status), GFP_KERNEL);
	if (!__irq_status) {
		flux_debug("failed to alloc irq status\n");
		return;
	}
	for (i = 0; i < NR_CPUS; i++) {
		irq_status = &__irq_status[i];
		irq_status->index = 0;
		memset(irq_status->bits, 0, sizeof(irq_status->bits));
	}

	irq_users = kmalloc_array(NR_IRQS, sizeof(char *), GFP_KERNEL);
	if (!irq_users) {
		flux_debug("failed to alloc irq users\n");
		kfree(__irq_status);
		return;
	}

	set_bit(0, irq_alloc); /* 0 is reserved */

	for (i = FLUX_IRQ_BASE; i < NR_IRQS; i++)
		irq_set_chip_and_handler(i, &dummy_irq_chip, handle_simple_irq);

	pr_info("flux: irqs initialized\n");
}

void cpu_yield_to_irqs(void)
{
	cpu_relax();
}

/*
 * /proc/interrupts printing for arch specific interrupts
 */
int arch_show_interrupts(struct seq_file *p, int prec)
{
	return 0;
}

/* irq interfaces */

static __always_inline void __set_irq_pending(int cpu, int irq)
{
	int index = irq / IRQ_STATUS_BITS;
	int bit = irq % IRQ_STATUS_BITS;

	__sync_fetch_and_or(&__irq_status[cpu].bits[index], 1UL << bit);
	__sync_fetch_and_or(&__irq_status[cpu].index, 1UL << index);
	/* Set irqs_pending for the target CPU, not the current CPU.
	 * Use direct pointer access since per_cpu macros expect lvalue. */
	*per_cpu_ptr(&tls_pcpu.irqs_pending, cpu) = true;
}

void flux_set_irq_pending(int irq)
{
	__set_irq_pending(smp_processor_id(), irq);
}

void flux_set_remote_irq_pending(int cpu, int irq)
{
	__set_irq_pending(cpu, irq);
}

int flux_get_free_irq(const char *user)
{
	int i;
	int ret = -EBUSY;

	i = find_first_zero_bit(irq_alloc, NR_IRQS);
	if (i >= NR_IRQS) {
		pr_err("flux: no free irq\n");
		return ret;
	}

	if (test_and_set_bit(i, irq_alloc)) {
		pr_err("flux: irq %d already allocated\n", i);
		return ret;
	}

	irq_users[i] = user;

	return i;
}

void flux_put_irq(int irq)
{
	if (irq < 0 || irq >= NR_IRQS) {
		pr_err("flux: invalid irq %d\n", irq);
		return;
	}

	irq_users[irq] = NULL;
	clear_bit(irq, irq_alloc);
}

static struct pt_regs dummy;

static void run_irq(int irq)
{
	unsigned long flags;
	struct pt_regs *old_regs;

	old_regs = set_irq_regs((struct pt_regs *)&dummy);

	/* interrupt handlers need to run with interrupts disabled */
	local_irq_save(flags);

	irq_enter();
	generic_handle_irq(irq);
#ifdef CONFIG_SMP
	if (FLUX_IRQ_IPI == irq)
		flux_ipi();
#endif
	irq_exit();

#ifdef CONFIG_SMP
	if (FLUX_IRQ_IPI == irq)
		flux_scheduler_ipi();
#endif

	set_irq_regs(old_regs);

	local_irq_restore(flags);
}

static inline unsigned long test_and_clear_irq_index_status(void)
{
	if (!current_irq_status->index)
		return 0;
	return __sync_fetch_and_and(&current_irq_status->index, 0);
}

static inline unsigned long test_and_clear_irq_status(int index)
{
	if (!current_irq_status->bits[index])
		return 0;
	return __sync_fetch_and_and(&current_irq_status->bits[index], 0);
}

static inline void for_each_bit(unsigned long word, void (*f)(int, int), int j)
{
	int i = 0;

	while (word) {
		if (word & 1)
			f(i, j);
		word >>= 1;
		i++;
	}
}

static inline void deliver_irq(int bit, int index)
{
	run_irq(index * IRQ_STATUS_BITS + bit);
}

static inline void check_irq_status(int i, int unused)
{
	for_each_bit(test_and_clear_irq_status(i), deliver_irq, i);
}

void flux_run_irqs(void)
{
	if (raw_cpu_read(tls_pcpu.irqs_pending)) {
		while (raw_cpu_cmpxchg(tls_pcpu.irqs_pending, true, false)) {
			for_each_bit(test_and_clear_irq_index_status(),
				     check_irq_status, 0);
		}
	}
}
