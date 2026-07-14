/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_UACCESS_H
#define _ASM_FLUX_UACCESS_H

#ifdef CONFIG_X86_64
#include <asm/x86/uaccess.h>
#else
#error "Unsupported architecture for uaccess implemtations"
#endif

/* CONFIG_UACCESS_MEMCPY */

#include <asm/unaligned.h>

static __always_inline int __get_user_fn(size_t size, const void __user *from,
					 void *to)
{
	BUILD_BUG_ON(!__builtin_constant_p(size));

	switch (size) {
	case 1:
		*(u8 *)to = *((u8 __force *)from);
		return 0;
	case 2:
		*(u16 *)to = get_unaligned((u16 __force *)from);
		return 0;
	case 4:
		*(u32 *)to = get_unaligned((u32 __force *)from);
		return 0;
	case 8:
		*(u64 *)to = get_unaligned((u64 __force *)from);
		return 0;
	default:
		BUILD_BUG();
		return 0;
	}
}
#define __get_user_fn(sz, u, k) __get_user_fn(sz, u, k)

static __always_inline int __put_user_fn(size_t size, void __user *to,
					 void *from)
{
	BUILD_BUG_ON(!__builtin_constant_p(size));

	switch (size) {
	case 1:
		*(u8 __force *)to = *(u8 *)from;
		return 0;
	case 2:
		put_unaligned(*(u16 *)from, (u16 __force *)to);
		return 0;
	case 4:
		put_unaligned(*(u32 *)from, (u32 __force *)to);
		return 0;
	case 8:
		put_unaligned(*(u64 *)from, (u64 __force *)to);
		return 0;
	default:
		BUILD_BUG();
		return 0;
	}
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
