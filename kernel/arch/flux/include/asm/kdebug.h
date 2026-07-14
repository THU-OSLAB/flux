/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KDEBUG_H
#define _ASM_X86_KDEBUG_H

#include <linux/notifier.h>

struct pt_regs;

/* Grossly misnamed. */
enum die_val {
	DIE_OOPS = 1,
};

enum show_regs_mode {
	SHOW_REGS_SHORT,
	/*
	 * For when userspace crashed, but we don't think it's our fault, and
	 * therefore don't print kernel registers.
	 */
	SHOW_REGS_USER,
	SHOW_REGS_ALL
};

#endif /* _ASM_X86_KDEBUG_H */
