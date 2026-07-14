/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_CMPXCHG_H
#define _ASM_FLUX_CMPXCHG_H

#ifdef CONFIG_X86_64
#include <asm/x86/cmpxchg.h>
#else
#error "Unsupported architecture for cmpxchg implemtations"
#endif

#endif /* _ASM_FLUX_CMPXCHG_H */
