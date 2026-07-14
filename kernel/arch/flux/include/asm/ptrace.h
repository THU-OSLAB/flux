#ifndef _ASM_FLUX_PTRACE_H
#define _ASM_FLUX_PTRACE_H

#ifdef CONFIG_X86_64
#include <asm/x86/ptrace.h>
#else
#error ""
#endif

#include <linux/errno.h>

struct task_struct;

static inline long arch_ptrace(struct task_struct *child, long request,
			       unsigned long addr, unsigned long data)
{
	return -EINVAL;
}

static inline void ptrace_disable(struct task_struct *child)
{
}

#endif
