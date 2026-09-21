/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_IOPORT_H
#define _ASM_FLUX_IOPORT_H

#include <linux/signal.h>

struct insn;
struct task_struct;

#ifdef CONFIG_FLUX_MPK
bool flux_io_is_insn(const struct insn *insn);
bool flux_io_handle(int *sig, siginfo_t *si, void *context,
		    const struct insn *insn);
bool flux_io_signal(int *sig, siginfo_t *si, void *context);
void flux_io_bitmap_share(struct task_struct *task);
void flux_io_bitmap_exit(struct task_struct *task);
#endif

#endif
