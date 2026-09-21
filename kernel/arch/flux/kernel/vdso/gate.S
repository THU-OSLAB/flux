/* SPDX-License-Identifier: GPL-2.0 */

#define FLUX_SYSCALL_GATE 0x100000

#include <asm/unistd.h>

	.text

	.macro flux_vdso_call3 name, nr
	.globl \name
	.type \name, @function
\name:
	endbr64
	movq %rdx, %rcx
	movq %rsi, %rdx
	movq %rdi, %rsi
	movl $\nr, %edi
	jmp *FLUX_SYSCALL_GATE
	.size \name, .-\name
	.endm

	.globl __flux_vdso_syscall2
	.hidden __flux_vdso_syscall2
	.type __flux_vdso_syscall2, @function
__flux_vdso_syscall2:
	endbr64
	jmp *FLUX_SYSCALL_GATE
	.size __flux_vdso_syscall2, .-__flux_vdso_syscall2

	flux_vdso_call3 __vdso_getcpu, __NR_getcpu
	.weak getcpu
	.set getcpu, __vdso_getcpu

	.section .note.GNU-stack,"",@progbits
