/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_PREEMPT_H
#define _ASM_FLUX_PREEMPT_H

#ifdef CONFIG_X86_64
#include <asm/x86/preempt.h>
#else
#include <asm-generic/preempt.h>
#endif

#endif /* _ASM_FLUX_PREEMPT_H */