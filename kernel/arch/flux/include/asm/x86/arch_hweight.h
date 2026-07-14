/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_HWEIGHT_H
#define _ASM_X86_HWEIGHT_H

#include <asm-generic/int-ll64.h>
#include <asm/x86/cpufeatures.h>

#define REG_IN "D"
#define REG_OUT "a"

static __always_inline unsigned int __arch_hweight32(unsigned int w)
{
	unsigned int res;

	asm("popcntl %1, %0" : "=" REG_OUT(res) : REG_IN(w));

	return res;
}

static inline unsigned int __arch_hweight16(unsigned int w)
{
	return __arch_hweight32(w & 0xffff);
}

static inline unsigned int __arch_hweight8(unsigned int w)
{
	return __arch_hweight32(w & 0xff);
}

static __always_inline unsigned long __arch_hweight64(u64 w)
{
	unsigned long res;

	asm("popcntq %1, %0" : "=" REG_OUT(res) : REG_IN(w));

	return res;
}

#endif
