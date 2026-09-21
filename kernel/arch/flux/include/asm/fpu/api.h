/* SPDX-License-Identifier: GPL-2.0 */
/* Minimal FPU API exposed to generic x86 users of Flux's FPU support. */
#ifndef _ASM_FLUX_FPU_API_H
#define _ASM_FLUX_FPU_API_H

#include <asm/x86/fpu.h>
#include <asm/x86/cpufeature.h>

#define KFPU_387 _BITUL(0)
#define KFPU_MXCSR _BITUL(1)

extern void kernel_fpu_begin(void);
extern void kernel_fpu_end(void);

static inline bool irq_fpu_usable(void)
{
	return true;
}

static inline void kernel_fpu_begin_mask(unsigned int kfpu_mask)
{
	(void)kfpu_mask;
	kernel_fpu_begin();
}

#endif /* _ASM_FLUX_FPU_API_H */
