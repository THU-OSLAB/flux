/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/percpu.h>
#include <asm/thread_info.h>
#include <uapi/asm/mpk.h>
#include <uapi/asm/unistd_64.h>

#ifdef CONFIG_FLUX_MPK
.macro FLUX_MPK_ENTER_UINTR
	xorl %ecx, %ecx
	.byte 0x0f, 0x01, 0xee /* rdpkru */
	movl %eax, %esi
	xorl %eax, %eax
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	testl %eax, %eax
	jne .Lflux_mpk_bad_pkru
.endm

.macro FLUX_MPK_ENTER_SYSCALL
	pushq %rax
	pushq %rcx
	pushq %rdx
	xorl %eax, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	popq %rdx
	popq %rcx
	popq %rax
.endm

.macro FLUX_MPK_ENTER_SYSCALL_FAST
	movq %rcx, %r10
	movq %rdx, %r11
	xorl %eax, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	movq %r10, %rcx
	movq %r11, %rdx
.endm

.macro FLUX_MPK_EXIT_USER
	/* Preserve the value at (%rsp); scratch space is below it. */
	pushq %rax
	pushq %rcx
	pushq %rdx
	movl $FLUX_MPK_APP_PKRU, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_APP_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
	popq %rdx
	popq %rcx
	popq %rax
.endm

.macro FLUX_MPK_EXIT_USER_FAST
	movq %rax, %r10
	movl $FLUX_MPK_APP_PKRU, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_APP_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
	movq %r10, %rax
.endm

/*
 * Build a two-stage return frame below the application red zone:
 *
 *   saved rdx, saved rcx, saved rax, final rip, final flags, final rsp
 *
 * The first UIRET enters flux_mpk_user_return with UIF clear. The
 * trampoline lowers PKRU, restores the three WRPKRU operand registers,
 * then the second UIRET atomically commits the final user context.
 */
.macro FLUX_MPK_PREPARE_USER_RETURN regs tmp val
	movq REG_rsp(\regs), \tmp
	subq $(128 + 48), \tmp
	andq $-16, \tmp

	movq REG_rdx(\regs), \val
	movq \val, 0x00(\tmp)
	movq REG_rcx(\regs), \val
	movq \val, 0x08(\tmp)
	movq REG_rax(\regs), \val
	movq \val, 0x10(\tmp)
	movq REG_rip(\regs), \val
	movq \val, 0x18(\tmp)
	movq REG_flags(\regs), \val
	movq \val, 0x20(\tmp)
	movq REG_rsp(\regs), \val
	movq \val, 0x28(\tmp)

	leaq flux_mpk_user_return(%rip), \val
	movq \val, REG_rip(\regs)
	movq \tmp, REG_rsp(\regs)
	btrq $32, REG_flags(\regs)
.endm

.pushsection .text, "ax"
.Lflux_mpk_bad_pkru:
	ud2
.popsection
#endif

/* Save callee-saved registers */
.macro PUSH_CALLEE
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
.endm

/* Restore callee-saved registers */
.macro POP_CALLEE
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
.endm

/* Save all registers except RSP */
.macro PUSH_ALL
	pushq %rax
	pushq %rdi
	pushq %rsi
	pushq %rdx
	pushq %rcx
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %rbx
	pushq %rbp
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
.endm

/* Restore all registers except RSP */
.macro POP_ALL
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbp
	popq %rbx
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rcx
	popq %rdx
	popq %rsi
	popq %rdi
	popq %rax
.endm

/* No return, just halt the CPU. */
.macro NORET
	hlt
.endm

/*
 * %rdi: prev task
 * %rsi: next task
 */
.pushsection .text, "ax"
SYM_FUNC_START(__switch_to_asm)
	/* This must match the order in inactive_task_frame */
	PUSH_CALLEE
	movq %rsp, TASK_thread_sp(%rdi)
	movq TASK_thread_sp(%rsi), %rsp
	POP_CALLEE
	jmp	__switch_to
SYM_FUNC_END(__switch_to_asm)
.popsection

/*
 * A newly forked process directly context switches into this address.
 *
 * rax: prev task we switched from
 * rbx: kernel thread func (NULL for user thread)
 * r12: kernel thread arg
 */
.pushsection .text, "ax"
SYM_CODE_START(ret_from_fork_asm)
	movq %rax, %rdi		/* prev */
	movq %rsp, %rsi		/* regs */
	movq %rbx, %rdx		/* fn */
	movq %r12, %rcx		/* fn_arg */
	andq $-16, %rsp
	call ret_from_fork
SYM_CODE_END(ret_from_fork_asm)
.popsection

/*
 * Switch from Flux kernel to user after a syscall.
 *
 * rdi: pt_regs
 */
.pushsection .text, "ax"
SYM_CODE_START(syscall_ret_to_user)
#ifdef CONFIG_FLUX_MPK
	FLUX_MPK_PREPARE_USER_RETURN %rdi %rax %rcx
#endif
	movq %rdi, %rsp
	POP_ALL
	addq $0x10, %rsp /* skip orig_ax and uirrv */
	uiret
SYM_CODE_END(syscall_ret_to_user)

/* Flux Env stays in the trusted MPK domain across its syscall wrapper. */
SYM_CODE_START(syscall_ret_to_env)
	movq %rdi, %rsp
	POP_ALL
	addq $0x10, %rsp /* skip orig_ax and uirrv */
	uiret
SYM_CODE_END(syscall_ret_to_env)

#ifdef CONFIG_FLUX_MPK
.pushsection .text, "ax"
SYM_CODE_START(flux_mpk_user_return)
	movl $FLUX_MPK_APP_PKRU, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_APP_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
	popq %rdx
	popq %rcx
	popq %rax
	uiret
SYM_CODE_END(flux_mpk_user_return)
.popsection
#endif
.popsection

.macro SYSCALL_PUSH_REGS
	pushq %rsp
	pushfq /* save rflags */
	subq $0x18, %rsp /* skip ip, uirrv and orig_ax */
	PUSH_ALL
.endm

.macro PER_CPU_VAR base offset reg
	leaq \base(%rip), \reg
	movq %gs:\offset(\reg), \reg
.endm

/*
 * Entry point for syscall from user.
 *
 * rax: syscall number
 * rdi, rsi, rdx, r10, r8, r9: args 1~6
 */
.pushsection .text, "ax"
SYM_FUNC_START(flux_syscall)
#ifdef CONFIG_FLUX_MPK
	FLUX_MPK_ENTER_SYSCALL
#endif
	/* retrieve the kernel stack */
	PER_CPU_VAR tls_pcpu PCPU_stack_top %r11

	/* save user rsp */
	subq $0x8, %r11
	movq %rsp, (%r11)
	
	/* switch to the kernel stack */
	movq %r11, %rsp

	/* user rsp already saved */
	pushfq /* save rflags */
	subq $0x18, %rsp /* skip ip, uirrv and orig_ax */
	PUSH_ALL

	movq %rax, %rsi
	movq %rsp, %rdi
	andq $-16, %rsp
	call syscall_entry_slow
	NORET
SYM_FUNC_END(flux_syscall)
.popsection

/*
 * Entry point for Flux Env syscalls, already
 * running on a kernel stack.
 *
 * rdi: syscall number
 * rsi, rdx, rcx, r8, r9, 0x8(rsp): args 1~6
 */
.pushsection .text, "ax"
SYM_FUNC_START(flux_syscall_env)
	movq %rdi, %rax /* save again for syscall_get_nr */
	SYSCALL_PUSH_REGS
	movq %rdi, %rsi
	movq %rsp, %rdi
	andq $-16, %rsp
	call syscall_entry_env
	NORET
SYM_FUNC_END(flux_syscall_env)
.popsection

/*
 * Fast path for syscall from user. We don't build pt_regs on
 * the top of the kernel stack. Syscalls building signal context
 * or cloning task context are not supported in this path.
 *
 * rdi: syscall number
 * rsi, rdx, rcx, r8, r9, 0x8(rsp): args 1~6
 */
.pushsection .text, "ax"
SYM_FUNC_START(flux_syscall_fast)
#ifdef CONFIG_FLUX_MPK
	FLUX_MPK_ENTER_SYSCALL_FAST
#endif
	/* retrieve the kernel stack */
	PER_CPU_VAR tls_pcpu PCPU_stack_top %r11

	/* save user rsp */
	subq $0x8, %r11
	movq %rsp, (%r11)
	
	/* save arg6 */
	movq 0x8(%rsp), %r10

	/* switch to the kernel stack */
	movq %r11, %rsp

	/* 
	 * uintr at this point will not be handled until in_kernel
	 * flag is already set.
	 */

	/* user rsp already saved */
	pushfq /* save rflags */
	subq $0x18, %rsp /* skip ip, uirrv and orig_ax */
	PUSH_ALL

	/* 
	 * We already know that struct pt_regs is 160B, which is 
	 * 16B aligned. So we just align the stack with a constant
	 * subq $0x8, %rsp
	 */
	subq $0x8, %rsp
	/* push arg6 */
	pushq %r10 

	call syscall_entry_fast

	/* restore user rsp */
	addq $0x10, %rsp
	movq REG_rsp(%rsp), %rsp

	/*
	 * We shift the stack pointer in syscall_prepare_regs. REDZONE
	 * will be reserved before entering uintr handler here, so a
	 * uintr at this point is ok.
	 */
	subq $0x8, %rsp

#ifdef CONFIG_FLUX_MPK
	/* %rsp points at the return address; this macro does not touch it. */
	FLUX_MPK_EXIT_USER_FAST
#endif

	/* just return without pop all registers */
	ret
SYM_FUNC_END(flux_syscall_fast)
.popsection

/*
 * Perform syscall on host.
 *
 * rdi: syscall number
 * rsi, rdx, rcx, r8, r9, 0x8(rsp): args 1~6
 */
.pushsection .text, "ax"
SYM_FUNC_START(host_syscall)
	movq %rdi, %rax
	movq %rsi, %rdi
	movq %rdx, %rsi
	movq %rcx, %rdx
	movq %r8,  %r10
	movq %r9,  %r8
	movq 0x8(%rsp), %r9
	syscall
	ret
SYM_FUNC_END(host_syscall)
.popsection

#ifdef CONFIG_FLUX_UINTR
.pushsection .text, "ax"
SYM_CODE_START(flux_uintr_handler)
	subq $0x8, %rsp /* skip orig_ax */
	PUSH_ALL
#ifdef CONFIG_FLUX_MPK
	/* Pass the interrupted PKRU as uintr_handler()'s second argument. */
	FLUX_MPK_ENTER_UINTR
#endif

	/* pt_regs as the first argument */
	movq %rsp, %rdi

	/*
	 * Save the interrupted frame before testing the current stack.
	 *
	 * init_task uses a static stack outside the mem block range. Nested
	 * UINTRs can therefore arrive while we are already running on the
	 * per-CPU UINTR stack. Resetting %rsp to uintr_stack_top again in that
	 * case would overwrite the saved frame we just built above.
	 * 
	 * So we must use a more precise range than [memory_start, memory_end)
	 * to determine if we are on the per-CPU UINTR stack.
	 *
	 * See asm/x86/processor.h and x86/process.c:__switch_to() for 
	 * more details about the stack layout.
	 */
	PER_CPU_VAR tls_pcpu PCPU_uintr_stack_top %r11
	leaq -(THREAD_SIZE / 2)(%r11), %r10
	cmpq %r10, %rsp
	jb .Lswitch_to_uintr_stack
	cmpq %r11, %rsp
	jb .Lcall_uintr_handler

.Lswitch_to_uintr_stack:
	movq %r11, %rsp

.Lcall_uintr_handler:
	andq $-16, %rsp
	call uintr_handler
	NORET
SYM_CODE_END(flux_uintr_handler)
.popsection

/*
 * UINTR handler switches into this address.
 *
 * rdi: UINTR context
 */
.pushsection .text, "ax"
SYM_CODE_START(__ret_from_uintr)
	movq %rdi, %rsp
#ifdef CONFIG_FLUX_MPK
	testl %esi, %esi
	jz .Luintr_ret_kernel
	cmpl $FLUX_MPK_APP_PKRU, %esi
	jne .Lflux_mpk_bad_pkru
	FLUX_MPK_EXIT_USER
.Luintr_ret_kernel:
#endif
	POP_ALL
	add $0x10, %rsp /* skip orig_ax and uirrv */
	uiret
SYM_CODE_END(__ret_from_uintr)
.popsection

#endif /* CONFIG_FLUX_UINTR */