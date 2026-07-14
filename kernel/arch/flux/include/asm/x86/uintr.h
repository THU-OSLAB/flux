#ifndef _ASM_X86_UINTR_H
#define _ASM_X86_UINTR_H

#ifndef __ASSEMBLY__

/**
 * uintr_state - state of the CPU at the time of a UINTR.
 *
 * This structure is used to save the state of the CPU when a UINTR
 * is delivered. The state is saved on the stack and passed to the
 * uintr_handler function. The layout of this structure must be the
 * same as the layout of struct pt_regs.
 * 
 *    _________  <- RSP @ UINTR
 *   |---------| <- 16B align
 *   | holdRSP |
 *   | RFLAGS  |
 *   | RIP     | <- RSP @ UIRET
 *   | UIRRV   | <- RSP @ __uintr_entry
 *   |---------|
 *   | rax~r15 | <- uintr_state
 *   |---------| <- 16B align
 *   |_________| <- RSP @ uintr_handler
 */

void __uintr_handler(void);
#ifdef CONFIG_FLUX_MPK
__noreturn void __ret_from_uintr(struct pt_regs *regs,
				 unsigned int interrupted_pkru);
asmlinkage __noreturn void uintr_handler(struct pt_regs *regs,
					 unsigned int interrupted_pkru);
#else
__noreturn void __ret_from_uintr(struct pt_regs *regs);
asmlinkage __noreturn void uintr_handler(struct pt_regs *regs);
#endif

static __always_inline void senduipi(u64 index)
{
	asm volatile("senduipi %0" : : "r"(index) : "memory");
}
void flux_uintr_send_ipi(int from, int to, int vector);
void flux_uintr_send_uvec(int from, int to, int vector);

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_X86_UINTR_H */