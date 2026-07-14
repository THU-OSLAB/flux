/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CURRENT_H
#define _ASM_X86_CURRENT_H

#include <linux/compiler.h>

#ifndef __ASSEMBLY__

#include <linux/cache.h>
#include <asm/percpu.h>

struct task_struct;
struct thread_struct;

struct tls_pcpu {
	union {
		struct {
			struct task_struct *current_task;
			unsigned long stack_top;
			unsigned long uintr_stack_top;
			int preempt_count;
			int cpu_number;
			int current_proc_key;
			int host_tid;
			bool irqs_pending;
			bool in_kernel;
		};
		u8 pad0[64];
	};
};
static_assert(sizeof(struct tls_pcpu) == 64);

DECLARE_PER_CPU_ALIGNED(struct tls_pcpu, tls_pcpu);

static __always_inline struct task_struct *get_current(void)
{
	return this_cpu_read(tls_pcpu.current_task);
}

#define current get_current()

static __always_inline int current_proc_key(void)
{
	return this_cpu_read(tls_pcpu.current_proc_key);
}

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CURRENT_H */