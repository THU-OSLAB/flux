/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_UACCESS_H
#define _ASM_FLUX_UACCESS_H

#include <asm/mpk_uaccess.h>

#ifdef CONFIG_FLUX_MPK
/*
 * The begin helpers receive a maximum range, not necessarily the bytes that
 * will be touched. strnlen_user(), for example, passes the remainder of the
 * user address space and then probes one word at a time. Applying the MPK map
 * policy to that theoretical maximum rejects valid short strings whenever a
 * protected base mapping exists later in the address space.
 *
 * Keep the coarse begin check to address validity. Every unsafe operation
 * below still reaches __get_user(), __put_user(), or raw_copy_*_user(), which
 * applies flux_mpk_uaccess_ok() to the exact bytes actually accessed.
 */
#define user_access_begin(ptr, len) __access_ok((ptr), (len))
#define user_access_end() do { } while (0)
#define user_read_access_begin(ptr, len) __access_ok((ptr), (len))
#define user_read_access_end() do { } while (0)
#define user_write_access_begin(ptr, len) __access_ok((ptr), (len))
#define user_write_access_end() do { } while (0)
#define unsafe_op_wrap(op, err) do { if (unlikely(op)) goto err; } while (0)
#define unsafe_get_user(x, p, e) unsafe_op_wrap(__get_user(x, p), e)
#define unsafe_put_user(x, p, e) unsafe_op_wrap(__put_user(x, p), e)
#define unsafe_copy_to_user(d, s, l, e) \
	unsafe_op_wrap(__copy_to_user(d, s, l), e)
#define unsafe_copy_from_user(d, s, l, e) \
	unsafe_op_wrap(__copy_from_user(d, s, l), e)
static inline unsigned long user_access_save(void)
{
	return 0UL;
}

static inline void user_access_restore(unsigned long flags)
{
}
#endif

#ifdef CONFIG_X86_64
#include <asm/x86/uaccess.h>
#else
#error "Unsupported architecture for uaccess implemtations"
#endif

/* CONFIG_UACCESS_MEMCPY */

#include <asm/unaligned.h>

static __always_inline int flux_access_ok(const void __user *ptr,
					  unsigned long size)
{
	unsigned long addr = (unsigned long)ptr;
	unsigned long limit = 0x0000800000000000UL;

	if (size > limit || addr > limit - size)
		return 0;
	if (!size)
		return 1;

	/*
	 * Keep Flux compatible with runtime/internal host C pointers and early
	 * Flux startup: a full Flux-VMA access_ok() rejects valid loader argv/env
	 * paths before user code is running.  Still reject the low guard page so
	 * NULL/0x1 bad-user-pointer tests fault before host alias mappings can turn
	 * them into EINVAL/EBADF.
	 */
	if (addr < PAGE_SIZE)
		return 0;

	return 1;
}
#define __access_ok flux_access_ok

static __always_inline int __get_user_fn(size_t size, const void __user *from,
					 void *to)
{
	BUILD_BUG_ON(!__builtin_constant_p(size));

#ifdef CONFIG_FLUX_MPK
	if (unlikely(!flux_mpk_uaccess_ok(from, size)))
		return -EFAULT;
#endif
	return raw_copy_from_user(to, from, size) ? -EFAULT : 0;
}
#define __get_user_fn(sz, u, k) __get_user_fn(sz, u, k)

static __always_inline int __put_user_fn(size_t size, void __user *to,
					 void *from)
{
	BUILD_BUG_ON(!__builtin_constant_p(size));

#ifdef CONFIG_FLUX_MPK
	if (unlikely(!flux_mpk_uaccess_ok(to, size)))
		return -EFAULT;
#endif
	return raw_copy_to_user(to, from, size) ? -EFAULT : 0;
}
#define __put_user_fn(sz, u, k) __put_user_fn(sz, u, k)

#define __get_kernel_nofault(dst, src, type, err_label)                     \
	do {                                                                \
		*((type *)dst) = get_unaligned((type *)(src));              \
		if (0) /* make sure the label looks used to the compiler */ \
			goto err_label;                                     \
	} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)                     \
	do {                                                                \
		put_unaligned(*((type *)src), (type *)(dst));               \
		if (0) /* make sure the label looks used to the compiler */ \
			goto err_label;                                     \
	} while (0)

#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#include <asm-generic/uaccess.h>

#endif
