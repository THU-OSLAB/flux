/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2002 Andi Kleen */

#include <linux/linkage.h>

.section .noinstr.text, "ax"

/*
 * memcpy - Copy a memory block.
 *
 * Input:
 *  rdi destination
 *  rsi source
 *  rdx count
 *
 * Output:
 * rax original destination
 */
SYM_FUNC_START(__memcpy)
	movq %rdi, %rax
	movq %rdx, %rcx
	rep movsb
	ret
SYM_FUNC_END(__memcpy)
EXPORT_SYMBOL(__memcpy)

SYM_FUNC_ALIAS(memcpy, __memcpy)
EXPORT_SYMBOL(memcpy)
