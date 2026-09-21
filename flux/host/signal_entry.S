#include <kernel/asm/mpk.h>

	.text
	.p2align 4
	.globl flux_host_signal_entry
	.type flux_host_signal_entry, @function
flux_host_signal_entry:
#ifdef CONFIG_FLUX_MPK
	/* The kmod signal hook must enter this pkey-0 stack with PKRU 0. */
	/* Preserve the live R8/R9 signal metadata across RDPKRU. */
	movq %rdx, %r10
	movq %rcx, %r11
	xorl %ecx, %ecx
	.byte 0x0f, 0x01, 0xee /* rdpkru */
	testl %eax, %eax
	jne .Lflux_host_signal_bad_pkru
	movq %r10, %rdx
	movq %r11, %rcx
#else
	/* Private fourth argument: no interrupted PKRU in an MPK-off build. */
	xorl %ecx, %ecx
#endif
	jmp flux_host_signal_handler

#ifdef CONFIG_FLUX_MPK
.Lflux_host_signal_bad_pkru:
	ud2
#endif
	.size flux_host_signal_entry, .-flux_host_signal_entry

	.section .note.GNU-stack,"",@progbits
