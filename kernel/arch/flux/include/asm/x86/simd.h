#ifndef _ASM_X86_SIMD_H
#define _ASM_X86_SIMD_H

#include <asm/x86/fpu.h>/* CONFIG_X86_64 */

/*
 * may_use_simd - whether it is allowable at this time to issue SIMD
 *                instructions or access the SIMD register file
 */
static inline bool may_use_simd(void)
{
	return true;
}

#endif /* _ASM_X86_SIMD_H */