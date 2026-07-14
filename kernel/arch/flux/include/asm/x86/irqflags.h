/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X86_IRQFLAGS_H_
#define _X86_IRQFLAGS_H_

#include <asm/x86/processor-flags.h>

#ifndef __ASSEMBLY__

#ifdef CONFIG_FLUX_UINTR

static __always_inline unsigned long native_save_flags(void)
{
	/*
	 * The ZF, OF, AF, PF, SF flags are cleared and the CF flags to 
	 * the value of the user interrupt flag.
	 */
	char flags;
	asm volatile("testui\n\t"
		     "setc %0\n\t"
		     : "=r"(flags)
		     :
		     : "memory", "cc");
	return (unsigned long)flags;
}

static __always_inline void native_irq_enable(void)
{
	asm volatile("stui" : : : "memory");
}

static __always_inline void native_irq_disable(void)
{
	asm volatile("clui" : : : "memory");
}

static __always_inline unsigned long arch_local_save_flags(void)
{
	return native_save_flags();
}

static __always_inline void arch_local_irq_disable(void)
{
	native_irq_disable();
}

static __always_inline void arch_local_irq_enable(void)
{
	native_irq_enable();
}

static __always_inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return flags == 0;
}

static __always_inline int arch_irqs_disabled(void)
{
	unsigned long flags = arch_local_save_flags();

	return arch_irqs_disabled_flags(flags);
}

static __always_inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;

	flags = arch_local_save_flags();
	arch_local_irq_disable();

	return flags;
}

static __always_inline void arch_local_irq_restore(unsigned long flags)
{
	if (!arch_irqs_disabled_flags(flags))
		arch_local_irq_enable();
}

#else

/* generic implementations */

#include <asm-generic/irqflags.h>

#endif

#endif /* !__ASSEMBLY__ */

#endif
