/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Access to user system call parameters and results
 *
 * Copyright (C) 2008-2009 Red Hat, Inc.  All rights reserved.
 *
 * See asm-generic/syscall.h for descriptions of what we must do here.
 */

#ifndef _ASM_X86_SYSCALL_H
#define _ASM_X86_SYSCALL_H

#include <uapi/linux/audit.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <asm/host_ops.h>
#include <asm/thread_info.h> /* for TS_COMPAT */
#include <asm/unistd.h>
#include <asm/x86/processor.h>

typedef long (*syscall_ptr_t)(long a1, ...);

extern const void *syscall_table[];

/*
 * Only the low 32 bits of orig_ax are meaningful, so we return int.
 * This importantly ignores the high bits on 64-bit, so comparisons
 * sign-extend the low 32 bits.
 */
static inline int syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->orig_ax;
}

static inline void syscall_rollback(struct task_struct *task,
				    struct pt_regs *regs)
{
	regs->ax = regs->orig_ax;
}

static inline long syscall_get_error(struct task_struct *task,
				     struct pt_regs *regs)
{
	unsigned long error = regs->ax;
	return IS_ERR_VALUE(error) ? error : 0;
}

static inline long syscall_get_return_value(struct task_struct *task,
					    struct pt_regs *regs)
{
	return regs->ax;
}

static inline void syscall_set_return_value(struct task_struct *task,
					    struct pt_regs *regs, int error,
					    long val)
{
	regs->ax = (long)error ?: val;
}

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
	{
		*args++ = regs->di;
		*args++ = regs->si;
		*args++ = regs->dx;
		*args++ = regs->r10;
		*args++ = regs->r8;
		*args = regs->r9;
	}
}

void syscall_ret_to_user(struct pt_regs *regs);
extern char __flux_syscall_ret_to_user_end[];

static __always_inline bool
flux_syscall_on_user_return_path(unsigned long ip)
{
	return ip >= (unsigned long)syscall_ret_to_user &&
	       ip < (unsigned long)__flux_syscall_ret_to_user_end;
}

void syscall_ret_to_env(struct pt_regs *regs);

long flux_fork(unsigned long clone_flags, unsigned long newsp,
	       void __user *parent_tidptr, void __user *child_tidptr,
	       unsigned long tls);

#endif
