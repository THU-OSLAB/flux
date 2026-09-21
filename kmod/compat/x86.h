/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FLUX_KMOD_COMPAT_X86_H
#define _FLUX_KMOD_COMPAT_X86_H

#include <asm/extable.h>
#include <asm/fpu/types.h>
#include <asm/thread_info.h>

#ifdef TIF_NOTIFY_SIGNAL
#define FLUX_TIF_NOTIFY_SIGNAL TIF_NOTIFY_SIGNAL
#else
#define FLUX_TIF_NOTIFY_SIGNAL TIF_SIGPENDING
#endif

/* Older host headers expose XSTATE_OP through fpu/internal.h. */
#if __has_include(<asm/fpu/internal.h>)
#include <asm/fpu/internal.h>
#endif
#ifdef XSTATE_OP
#define FLUX_XSTATE_OP XSTATE_OP
#else
#define FLUX_XSTATE_OP(op, st, lmask, hmask, err)                       \
	asm volatile("1:" op "\n\t"                                     \
		     "xor %[err], %[err]\n"                             \
		     "2:\n\t" _ASM_EXTABLE_TYPE(1b, 2b,                 \
						EX_TYPE_FAULT_MCE_SAFE) \
		     : [err] "=a"(err)                                  \
		     : "D"(st), "m"(*st), "a"(lmask), "d"(hmask)        \
		     : "memory")
#endif

#endif /* _FLUX_KMOD_COMPAT_X86_H */
