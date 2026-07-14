/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_SYSCALL_H
#define _ASM_FLUX_SYSCALL_H

#include <uapi/linux/audit.h>

static inline int syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_X86_64;
}

#ifdef CONFIG_X86_64
#include <asm/x86/syscall.h>
#else
#include <asm-generic/syscall.h>
#endif

#endif /* _ASM_FLUX_SYSCALL_H */
