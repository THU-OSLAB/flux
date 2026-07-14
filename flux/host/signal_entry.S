#include <kernel/asm/mpk.h>

	.text
	.p2align 4
	.globl flux_host_signal_entry
	.type flux_host_signal_entry, @function
flux_host_signal_entry:
	/* The kmod signal hook must enter this pkey-0 stack with PKRU 0. */
	movq %rdx, %r8
	movq %rcx, %r9
	xorl %ecx, %ecx
	.byte 0x0f, 0x01, 0xee /* rdpkru */
	testl %eax, %eax
	jne .Lflux_host_signal_bad_pkru
	movq %r8, %rdx
	movq %r9, %rcx
	jmp flux_host_signal_handler

.Lflux_host_signal_bad_pkru:
	ud2
	.size flux_host_signal_entry, .-flux_host_signal_entry

	.section .note.GNU-stack,"",@progbits
