/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_BARRIER_H
#define _ASM_FLUX_BARRIER_H

#ifdef CONFIG_X86_64
#include <asm/x86/barrier.h>
#else
#error "Unsupported architecture for barrier implemtations"
#endif

#endif
