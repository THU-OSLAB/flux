/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2002 Andi Kleen, SuSE Labs */

#include <linux/linkage.h>

.section .noinstr.text, "ax"

/*
 * ISO C memset - set a memory block to a byte value. This function uses fast
 * string to get better performance than the original function. The code is
 * simpler and shorter than the original function as well.
 *
 * rdi   destination
 * rsi   value (char)
 * rdx   count (bytes)
 *
 * rax   original destination
 */
SYM_FUNC_START(__memset)
	movq %rdi,%r9
	movb %sil,%al
	movq %rdx,%rcx
	rep stosb
	movq %r9,%rax
	RET
SYM_FUNC_END(__memset)
EXPORT_SYMBOL(__memset)

SYM_FUNC_ALIAS(memset, __memset)
EXPORT_SYMBOL(memset)
