#ifndef _ASM_FLUX_PTRACE_H
#define _ASM_FLUX_PTRACE_H

#ifdef CONFIG_X86_64
#include <asm/x86/ptrace.h>
#else
#error ""
#endif

#include <linux/errno.h>

struct task_struct;
int ptrace_request(struct task_struct *child, long request,
		   unsigned long addr, unsigned long data);

static inline long arch_ptrace(struct task_struct *child, long request,
			       unsigned long addr, unsigned long data)
{
	return ptrace_request(child, request, addr, data);
}

static inline void ptrace_disable(struct task_struct *child)
{
}

#endif
