/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_ATOMIC_H
#define _ASM_FLUX_ATOMIC_H

#ifdef CONFIG_X86_64
#include <asm/x86/atomic.h>
#else
#error "Unsupported architecture for atomic implemtations"
#endif

#endif
