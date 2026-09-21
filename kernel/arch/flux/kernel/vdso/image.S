/* SPDX-License-Identifier: GPL-2.0 */
	.section .rodata
	.balign 4096
	.globl flux_vdso_start, flux_vdso_end
flux_vdso_start:
	.incbin "arch/flux/kernel/vdso/vdso.so"
	.balign 4096
flux_vdso_end:
	.section .note.GNU-stack,"",@progbits
