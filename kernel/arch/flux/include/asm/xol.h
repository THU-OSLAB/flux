/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_XOL_H
#define _ASM_FLUX_XOL_H

#include <linux/mm.h>
#include <linux/signal.h>

struct pt_regs;
struct task_struct;
struct flux_deferred_fault;
struct flux_xol_state;

static inline bool flux_xol_required(unsigned long flags)
{
	return IS_ENABLED(CONFIG_FLUX_MPK) && (flags & VM_EXEC) &&
	       (flags & (VM_MAYSHARE | VM_WRITE));
}

#ifdef CONFIG_FLUX_MPK
bool flux_xol_signal(int *sig, siginfo_t *si, void *context);
void flux_xol_finish_fault(struct pt_regs *regs,
			   const struct flux_deferred_fault *fault);
void flux_xol_interrupt(struct pt_regs *regs);
void flux_xol_release(struct task_struct *task);
void __noreturn flux_xol_syscall(struct pt_regs *regs);
#else
static inline void flux_xol_release(struct task_struct *task) { }
#endif

#endif
