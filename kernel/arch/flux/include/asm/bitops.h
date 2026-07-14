/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_BITOPS_H
#define _ASM_FLUX_BITOPS_H

#ifndef _LINUX_BITOPS_H
#error only <linux/bitops.h> can be included directly
#endif

#ifdef CONFIG_X86_64
#include <asm/x86/bitops.h>
#else
#error "Unsupported architecture for bitops implemtations"
#endif

#endif
