/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_MMZONE_H
#define _ASM_FLUX_MMZONE_H

#ifdef CONFIG_X86_64
#include <asm/x86/mmzone_64.h>
#else
#error "Unsupported architecture"
#endif

#endif
