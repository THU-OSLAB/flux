/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_ENTRY_COMMON_H
#define _ASM_X86_ENTRY_COMMON_H

#include <asm/x86/fpu.h>

#ifdef CONFIG_FLUX_UINTR
static inline void arch_exit_to_user_mode_prepare(struct pt_regs *regs,
						  unsigned long ti_work)
{
	if (unlikely(ti_work & (1 << TIF_NEED_FPU_LOAD))) {
		restore_xstate(task_xstate(current));
		clear_thread_flag(TIF_NEED_FPU_LOAD);
	}
}
#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare
#endif

static inline void arch_exit_to_user_mode(void)
{
	this_cpu_write(tls_pcpu.in_kernel, false);
}
#define arch_exit_to_user_mode arch_exit_to_user_mode

static inline void arch_enter_from_user_mode(struct pt_regs *regs)
{
	this_cpu_write(tls_pcpu.in_kernel, true);
}
#define arch_enter_from_user_mode arch_enter_from_user_mode

#endif
