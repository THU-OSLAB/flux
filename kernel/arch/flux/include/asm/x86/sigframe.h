/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SIGFRAME_H
#define _ASM_X86_SIGFRAME_H

#include <uapi/asm/sigcontext.h>
#include <asm/siginfo.h>
#include <asm/ucontext.h>
#include <linux/compat.h>

struct rt_sigframe {
	char __user *pretcode;
	struct ucontext uc;
	struct siginfo info;
	/* fp state follows here */
};

#endif /* _ASM_X86_SIGFRAME_H */