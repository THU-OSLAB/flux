/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_ENTRY_COMMON_H
#define _ASM_X86_ENTRY_COMMON_H

#include <asm/x86/fpu.h>
#include <asm/host_ops.h>
#include <asm/x86/processor.h>

/*
 * Flux syscall entry is a normal CALL and does not change UIF.  Likewise, its
 * return is a JMP rather than UIRET, so the generic exit loop must preserve the
 * interrupted hardware state instead of applying the native-kernel IRQ
 * enable/disable protocol.  Real UINTR entry and local_irq_* retain their
 * hardware TESTUI/STUI/CLUI semantics.
 */
static inline void local_irq_enable_exit_to_user(unsigned long ti_work)
{
	(void)ti_work;
}
#define local_irq_enable_exit_to_user local_irq_enable_exit_to_user

static inline void local_irq_disable_exit_to_user(void)
{
}
#define local_irq_disable_exit_to_user local_irq_disable_exit_to_user

static __always_inline void flux_restore_user_fpstate(unsigned long ti_work)
{
	if (unlikely(ti_work & (1 << TIF_NEED_FPU_LOAD))) {
		if (unlikely(restore_xstate(task_xstate(current)))) {
			memcpy(task_xstate(current), &init_task_xstate, PAGE_SIZE);
			WARN_ON_ONCE(restore_xstate(task_xstate(current)));
		}
		clear_thread_flag(TIF_NEED_FPU_LOAD);
	}
}

static inline void arch_exit_to_user_mode_prepare(struct pt_regs *regs,
						  unsigned long ti_work)
{
	flux_restore_user_fpstate(ti_work);
	/* Only application returns use this hook; Env restores its captured TLS. */
	wrfsbase(current->thread.fsbase);
}
#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare

DECLARE_PER_CPU(bool, flux_host_tsc_disabled);

static inline void arch_exit_to_user_mode(void)
{
	if (unlikely(test_thread_flag(TIF_NOTSC)))
		flux_tsc_restore_user_mode(true);
	this_cpu_write(tls_pcpu.in_kernel, false);
}
#define arch_exit_to_user_mode arch_exit_to_user_mode

static inline void arch_enter_from_user_mode(struct pt_regs *regs)
{
	this_cpu_write(tls_pcpu.in_kernel, true);
	if (unlikely(this_cpu_read(flux_host_tsc_disabled)))
		flux_tsc_enter_kernel_mode();
}
#define arch_enter_from_user_mode arch_enter_from_user_mode

#endif
