/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CURRENT_H
#define _ASM_X86_CURRENT_H

#include <linux/compiler.h>

#ifndef __ASSEMBLY__

#include <linux/cache.h>
#include <asm/percpu.h>

struct task_struct;
struct thread_struct;

struct flux_uintr_entry_scratch {
	unsigned long rax;
	unsigned long r10;
	unsigned long r11;
	unsigned long hw_rsp;
	unsigned long fsbase;
	unsigned long __reserved;
	unsigned long rsi;
	unsigned long pkru;
};
static_assert(sizeof(struct flux_uintr_entry_scratch) == 64);

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
			unsigned long host_fsbase;
			u8 __reserved0;
			bool in_kernel;
			unsigned int env_syscall_depth;
			/* Live host signal frames belong to the CPU pthread. */
			unsigned int host_signal_depth;
		};
		u8 pad0[64];
	};
	union {
		struct {
			unsigned long uintr_entry_busy;
		};
		u8 pad1[64];
	};
	struct flux_uintr_entry_scratch uintr_entry;
};
static_assert(sizeof(struct tls_pcpu) == 64 * 3);

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
