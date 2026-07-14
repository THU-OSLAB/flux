/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SWITCH_TO_H
#define _ASM_X86_SWITCH_TO_H

#include <linux/sched/task_stack.h>

struct task_struct; /* one of the stranger aspects of C forward declarations */

asmlinkage struct task_struct *__switch_to_asm(struct task_struct *prev,
				    struct task_struct *next);

struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next);

asmlinkage void ret_from_fork_asm(void);
__visible void ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
			      int (*fn)(void *), void *fn_arg);

/*
 * This is the structure pointed to by thread.sp for an inactive task.  The
 * order of the fields must match the code in __switch_to_asm().
 */
struct inactive_task_frame {
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long bx;

	/*
	 * These two fields must be together.  They form a stack frame header,
	 * needed by get_frame_pointer().
	 */
	unsigned long bp;
	unsigned long ret_addr;
};

struct fork_frame {
	struct inactive_task_frame frame;
	struct pt_regs regs;
};

#define switch_to(prev, next, last)                         \
	do {                                                \
		((last) = __switch_to_asm((prev), (next))); \
	} while (0)

static inline void kthread_frame_init(struct inactive_task_frame *frame,
				      int (*fun)(void *), void *arg)
{
	frame->bx = (unsigned long)fun;
	frame->r12 = (unsigned long)arg;
}

static inline unsigned long encode_frame_pointer(struct pt_regs *regs)
{
	return (unsigned long)regs + 1;
}

#endif /* _ASM_X86_SWITCH_TO_H */
