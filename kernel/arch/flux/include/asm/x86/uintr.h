#ifndef _ASM_X86_UINTR_H
#define _ASM_X86_UINTR_H

/* Hardware frame and stack contract shared by real and synthetic UINTR. */
#define FLUX_UINTR_FRAME_QWORDS 4
#define FLUX_UINTR_FRAME_BYTES (FLUX_UINTR_FRAME_QWORDS * 8)
#define FLUX_UINTR_SYSV_REDZONE 128
#define FLUX_UINTR_ENTRY_SCRATCH 32
/* Protected synthetic entry also saves PKRU; keep the frame 16-byte aligned. */
#define FLUX_UINTR_SIGNAL_FRAME_BYTES \
	(FLUX_UINTR_FRAME_BYTES + FLUX_UINTR_ENTRY_SCRATCH + 16)
/* Ordinary kernel-fault return words remain live with UIF enabled. */
#define FLUX_UINTR_RETURN_SCRATCH 32
#define FLUX_UINTR_STACK_ADJUST \
	(FLUX_UINTR_SYSV_REDZONE + FLUX_UINTR_RETURN_SCRATCH + \
	 FLUX_UINTR_ENTRY_SCRATCH)

#ifdef __ASSEMBLY__
#define FLUX_UINTR_SIGNAL_VECTOR 2
#endif

#ifndef __ASSEMBLY__

/*
 * Tag the vector word of a software-built UINTR frame.  UIRET consumes only
 * the following RIP/RFLAGS/RSP words, so this does not alter return state.
 */
#define FLUX_UINTR_SYNTHETIC_FLAG (1ULL << 63)

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
 *   |---------| <- 64B align
 *   | xstate  |    PAGE_SIZE bytes
 *   |---------| <- RSP before calling uintr_handler
 *   | retaddr |
 *   |_________| <- RSP @ uintr_handler
 */

struct xregs_state;

void __uintr_handler(void);
struct flux_user_fault_exit;
__noreturn void __ret_from_uintr_user_exit(struct flux_user_fault_exit *context);
__noreturn void __ret_from_uintr_kernel_fault(void *context);
__noreturn void __ret_from_kernel_fault(struct pt_regs *regs);
#ifdef CONFIG_FLUX_MPK
__noreturn void __ret_from_uintr(struct pt_regs *regs,
				 unsigned int interrupted_pkru);
asmlinkage __noreturn void uintr_handler(struct pt_regs *regs,
					 unsigned int interrupted_pkru,
					 struct xregs_state *interrupted_xstate);
#else
__noreturn void __ret_from_uintr(struct pt_regs *regs);
asmlinkage __noreturn void uintr_handler(struct pt_regs *regs,
					 struct xregs_state *interrupted_xstate);
#endif

static __always_inline void senduipi(u64 index)
{
	asm volatile("senduipi %0" : : "r"(index) : "memory");
}
void flux_uintr_send_ipi(int from, int to, int vector);
void flux_uintr_send_uvec(int from, int to, int vector);
void flux_uintr_defer_signal_delivery(int cpu);

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_X86_UINTR_H */
