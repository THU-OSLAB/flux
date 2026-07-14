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

/*
 * Copy To/From Userspace
 */

static __always_inline __must_check unsigned long
copy_user_generic(void *to, const void *from, unsigned long len)
{
	asm volatile("1:\n\t"
		     "rep movsb\n\t"
		     "2:\n\t" _ASM_EXTABLE_UA(1b, 2b)
		     : "+c"(len), "+D"(to), "+S"(from), ASM_CALL_CONSTRAINT
		     :
		     : "memory", "rax");
	return len;
}

static __always_inline __must_check unsigned long
raw_copy_from_user(void *dst, const void __user *src, unsigned long size)
{
	return copy_user_generic(dst, (__force void *)src, size);
}

static __always_inline __must_check unsigned long
raw_copy_to_user(void __user *dst, const void *src, unsigned long size)
{
	return copy_user_generic((__force void *)dst, src, size);
}

/*
 * Zero Userspace.
 */

static __always_inline __must_check unsigned long
__clear_user(void __user *addr, unsigned long size)
{
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
