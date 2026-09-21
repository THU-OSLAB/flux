/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UACCESS_64_H
#define _ASM_X86_UACCESS_64_H

/*
 * User space memory access functions
 */
#include <linux/compiler.h>
#include <linux/lockdep.h>
#include <linux/kasan-checks.h>
#include <asm/x86/cpufeatures.h>
#include <asm/x86/asm.h>
#include <asm/mpk_uaccess.h>

/*
 * Copy To/From Userspace
 */

/*
 * A fixed-width user access needs one load and one store, not a REP setup.
 * Protect both instructions: either direction can touch the user pointer.
 * Leave RCX at the original size on fault and clear it only after the store,
 * preserving raw_copy_*_user's uncopied-byte result without widening access.
 */
#define FLUX_COPY_USER_SCALAR(insn, reg)                                  \
	asm volatile("1: " insn " (%%rsi), %%" reg "\n\t"                 \
		     "2: " insn " %%" reg ", (%%rdi)\n\t"                 \
		     "xor %%ecx, %%ecx\n\t"                                 \
		     "3:\n\t" _ASM_EXTABLE_UA(1b, 3b)                         \
		     _ASM_EXTABLE_UA(2b, 3b)                                 \
		     : "+c"(len), ASM_CALL_CONSTRAINT                        \
		     : "D"(to), "S"(from)                                   \
		     : "memory", "rax", "cc")

static __always_inline __must_check unsigned long
copy_user_generic(void *to, const void *from, unsigned long len)
{
	if (__builtin_constant_p(len)) {
		switch (len) {
		case 0:
			return 0;
		case 1:
			FLUX_COPY_USER_SCALAR("movb", "al");
			return len;
		case 2:
			FLUX_COPY_USER_SCALAR("movw", "ax");
			return len;
		case 4:
			FLUX_COPY_USER_SCALAR("movl", "eax");
			return len;
		case 8:
			FLUX_COPY_USER_SCALAR("movq", "rax");
			return len;
		}
	}

	asm volatile("1:\n\t"
		     "rep movsb\n\t"
		     "2:\n\t" _ASM_EXTABLE_UA(1b, 2b)
		     : "+c"(len), "+D"(to), "+S"(from), ASM_CALL_CONSTRAINT
		     :
		     : "memory", "rax");
	return len;
}
#undef FLUX_COPY_USER_SCALAR

static __always_inline __must_check unsigned long
raw_copy_from_user(void *dst, const void __user *src, unsigned long size)
{
#ifdef CONFIG_FLUX_MPK
	if (unlikely(!flux_mpk_uaccess_ok(src, size)))
		return size;
#endif
	return copy_user_generic(dst, (__force void *)src, size);
}

static __always_inline __must_check unsigned long
raw_copy_to_user(void __user *dst, const void *src, unsigned long size)
{
#ifdef CONFIG_FLUX_MPK
	if (unlikely(!flux_mpk_uaccess_ok(dst, size)))
		return size;
#endif
	return copy_user_generic((__force void *)dst, src, size);
}

/*
 * Zero Userspace.
 */

static __always_inline __must_check unsigned long
__clear_user(void __user *addr, unsigned long size)
{
#ifdef CONFIG_FLUX_MPK
	if (unlikely(!flux_mpk_uaccess_ok(addr, size)))
		return size;
#endif
	/*
	 * No memory constraint because it doesn't change any memory gcc
	 * knows about.
	 */
	asm volatile("1:\n\t"
		     "rep stosb\n\t"
		     "2:\n\t" _ASM_EXTABLE_UA(1b, 2b)
		     : "+c"(size), "+D"(addr), ASM_CALL_CONSTRAINT
		     : "a"(0));

	return size;
}
#define __clear_user __clear_user

#endif /* _ASM_X86_UACCESS_64_H */
