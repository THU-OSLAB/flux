/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/percpu.h>
#include <asm/page.h>
#include <asm/thread_info.h>
#include <asm/x86/uintr.h>
#include <asm/x86/processor-flags.h>
#include <uapi/asm/mpk.h>
#include <uapi/asm/unistd_64.h>

#ifdef CONFIG_FLUX_MPK

.macro FLUX_MPK_ENTER_SYSCALL
	/*
	 * A rewritten syscall reaches this entry through call *%rax.  Do not
	 * push WRPKRU operands on the application stack: the call already uses
	 * the top eight bytes of the red zone.  RCX and R11 are syscall-clobbered,
	 * so use them to preserve RAX and pack both halves of RDX while keeping
	 * EAX, ECX and EDX zero for WRPKRU.
	 */
	movq %rax, %r11
	movq %rdx, %rax
	movq %rdx, %rcx
	shlq $32, %rax
	shrq $32, %rcx
	shlq $32, %rcx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_KERNEL_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
	shrq $32, %rax
	movq %rcx, %rdx
	orq %rax, %rdx
	movq %r11, %rax
.endm

.macro FLUX_MPK_ENTER_SYSCALL_FAST
	movq %rcx, %r10
	movq %rdx, %r11
	xorl %eax, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_KERNEL_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
	movq %r10, %rcx
	movq %r11, %rdx
.endm

/*
 * Kmod reserves a 32-byte kernel-return area below the SysV red zone,
 * followed by a separate 32-byte entry area. The hardware frame starts below
 * both areas. Preserve the WRPKRU operands at positive frame offsets,
 * instead of pushing
 * below it.  This neither overwrites the hardware frame nor assumes that the
 * page below it is mapped.
 */
.macro FLUX_MPK_ENTER_UINTR_SCRATCH
	movq %rsi, 0x20(%rsp)
	movq %rax, 0x28(%rsp)
	movq %rcx, 0x30(%rsp)
	movq %rdx, 0x38(%rsp)
	xorl %ecx, %ecx
	.byte 0x0f, 0x01, 0xee /* rdpkru */
	movl %eax, %esi
	xorl %eax, %eax
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	testl %eax, %eax
	jne .Lflux_mpk_bad_pkru
	movq 0x38(%rsp), %rdx
	movq 0x30(%rsp), %rcx
	movq 0x28(%rsp), %rax
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
 * A rewritten syscall follows the Linux syscall ABI, so only RCX and R11
 * may be clobbered on return.  Preserve RAX and RDX without using the
 * application red zone while WRPKRU requires EAX, ECX and EDX as operands.
 */
.macro FLUX_MPK_EXIT_REWRITTEN_SYSCALL
	movq %rax, %r11
	movq %rdx, %rcx
	movl $FLUX_MPK_APP_PKRU, %eax
	shlq $32, %rdx
	orq %rdx, %rax
	shrq $32, %rcx
	shlq $32, %rcx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_APP_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
	shrq $32, %rax
	movq %rcx, %rdx
	orq %rax, %rdx
	movq %r11, %rax
.endm

/*
 * Return from a hardware UINTR without touching an application-controlled
 * stack. Entries are admitted only with prior UIF=1; UIRET re-enables it.
 * The saved frame retains the interrupted RIP, RFLAGS and RSP.
 * Shared pkey 2 is writable with the kernel PKRU and read-only with the
 * application PKRU.  Populate this CPU's slot while the kernel PKRU is
 * active, restore every register except the three WRPKRU operands, then
 * switch RSP to the slot before lowering PKRU.  One final UIRET restores the
 * user RIP, RFLAGS and RSP; no intermediate return or UIF transition exists.
 *
 * Slot layout: saved rdx, saved rcx, saved rax, final rip, final flags,
 * final rsp.
 */
.macro FLUX_MPK_UIRET_TO_USER regs tmp val

	movslq %gs:tls_pcpu+PCPU_cpu_number(%rip), \tmp
	shlq $6, \tmp
	addq $FLUX_MPK_RETURN_ADDR, \tmp

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

	movq \regs, %rsp
	POP_ALL

	/* Recompute the slot after restoring the user's general registers. */
	movslq %gs:tls_pcpu+PCPU_cpu_number(%rip), %rax
	shlq $6, %rax
	addq $FLUX_MPK_RETURN_ADDR, %rax
	movq %rax, %rsp

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
.endm

.pushsection .text, "ax"
.Lflux_mpk_bad_pkru:
	ud2
.popsection
#endif

/*
 * Return from a CALL-based syscall without involving UIF or writing the
 * application stack. Each CPU has an application-read-only flags word at
 * the start of a page. The preceding page is writable to the application
 * domain so hardware UINTR delivery from this temporary RSP has valid stack
 * space. Neither a concurrent mprotect of the signal stack nor an exhausted
 * application stack can prevent POPFQ and the final MOV/JMP return.
 *
 * Both the register slot and flags word remain read-only with application
 * PKRU. Return-path UINTRs use the existing kernel continuation and cannot
 * schedule another task through the live per-CPU return data.
 *
 * Slot layout: saved rdx, saved rcx, saved rax, final flags, final rsp,
 * final rip.
 */
.macro FLUX_RET_TO_USER_SLOT regs slot
	movq \regs, %rsp
	POP_ALL
	movq $FLUX_SYSCALL_FLAGS_ADDR + (\slot * FLUX_SYSCALL_FLAGS_STACK_SIZE), %rsp
#ifdef CONFIG_FLUX_MPK
	movl $FLUX_MPK_APP_PKRU, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	cmpl $FLUX_MPK_APP_PKRU, %eax
	jne .Lflux_mpk_bad_pkru
#endif
	movq FLUX_SYSCALL_RETURN_ADDR + (\slot * FLUX_MPK_RETURN_SLOT_SIZE) + 0x00, %rdx
	movq FLUX_SYSCALL_RETURN_ADDR + (\slot * FLUX_MPK_RETURN_SLOT_SIZE) + 0x08, %rcx
	movq FLUX_SYSCALL_RETURN_ADDR + (\slot * FLUX_MPK_RETURN_SLOT_SIZE) + 0x10, %rax
	popfq
	movq FLUX_SYSCALL_RETURN_ADDR + (\slot * FLUX_MPK_RETURN_SLOT_SIZE) + 0x20, %rsp
	jmp *FLUX_SYSCALL_RETURN_ADDR + (\slot * FLUX_MPK_RETURN_SLOT_SIZE) + 0x28
.endm

#define FLUX_RET_TO_USER_CODE_SLOT_SIZE 128
#define FLUX_RET_TO_USER_CODE_SLOT_SHIFT 7

.macro FLUX_RET_TO_USER regs tmp val

	/*
	 * The fixed runtime area is populated at startup. Prepare both pages
	 * before publishing register slots; if resolving a mapping fault moves
	 * the task, prepare the destination CPU's pages before continuing.
	 */
.Lflux_prepare_return_flags\@:
	movslq %gs:tls_pcpu+PCPU_cpu_number(%rip), \tmp
	shlq $FLUX_SYSCALL_FLAGS_STACK_SHIFT, \tmp
	addq $FLUX_SYSCALL_FLAGS_ADDR, \tmp
	movq REG_flags(\regs), \val
	movq \val, -(FLUX_UINTR_STACK_ADJUST + FLUX_UINTR_FRAME_BYTES)(\tmp)
	movq \val, 0x00(\tmp)
	movslq %gs:tls_pcpu+PCPU_cpu_number(%rip), \val
	shlq $FLUX_SYSCALL_FLAGS_STACK_SHIFT, \val
	addq $FLUX_SYSCALL_FLAGS_ADDR, \val
	cmpq \val, \tmp
	jne .Lflux_prepare_return_flags\@

	movslq %gs:tls_pcpu+PCPU_cpu_number(%rip), \tmp
	shlq $6, \tmp
	addq $FLUX_SYSCALL_RETURN_ADDR, \tmp

	movq REG_rdx(\regs), \val
	movq \val, 0x00(\tmp)
	movq REG_rcx(\regs), \val
	movq \val, 0x08(\tmp)
	movq REG_rax(\regs), \val
	movq \val, 0x10(\tmp)
	movq REG_flags(\regs), \val
	movq \val, 0x18(\tmp)
	movq REG_rsp(\regs), \val
	movq \val, 0x20(\tmp)
	movq REG_rip(\regs), \val
	movq \val, 0x28(\tmp)

	movl %gs:tls_pcpu+PCPU_cpu_number(%rip), %eax
	cmpl $CONFIG_FLUX_MAX_CPUS, %eax
	jae .Lflux_ret_bad_cpu\@
	shlq $FLUX_RET_TO_USER_CODE_SLOT_SHIFT, %rax
	leaq .Lflux_ret_slots\@(%rip), \val
	addq \val, %rax
	jmp *%rax

.Lflux_ret_bad_cpu\@:
	ud2

	.balign FLUX_RET_TO_USER_CODE_SLOT_SIZE, 0xcc
.Lflux_ret_slots\@:
	.set .Lflux_ret_slot_index\@, 0
	.rept CONFIG_FLUX_MAX_CPUS
		.set .Lflux_ret_slot_start\@, .
		FLUX_RET_TO_USER_SLOT \regs .Lflux_ret_slot_index\@
		.org .Lflux_ret_slot_start\@ + FLUX_RET_TO_USER_CODE_SLOT_SIZE, 0xcc
		.set .Lflux_ret_slot_index\@, .Lflux_ret_slot_index\@ + 1
	.endr
.endm

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
	FLUX_RET_TO_USER %rdi %rax %rcx
	.globl __flux_syscall_ret_to_user_end
__flux_syscall_ret_to_user_end:
SYM_CODE_END(syscall_ret_to_user)


/*
 * Flux Env enters through a regular function call on the kernel stack.
 * regs->uirrv carries the host FS base captured at flux_syscall_env entry.
 */
SYM_CODE_START(syscall_ret_to_env)
	movq %rdi, %rsp
	POP_ALL
	movq 0x8(%rsp), %r11 /* saved host fsbase in regs->uirrv */
	wrfsbase %r11
	movq 0x10(%rsp), %r11 /* saved user ip */
	movq 0x20(%rsp), %rsp /* saved user sp, already past call return addr */
	subq $0x8, %rsp
	movq %r11, (%rsp)
	ret
SYM_CODE_END(syscall_ret_to_env)
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

.macro PER_CPU_DIRECT_LOAD base offset dst
	movq %gs:\base+\offset(%rip), \dst
.endm

.macro UINTR_SAVE_ENTRY
	lock btsq $0, %gs:tls_pcpu+PCPU_uintr_entry_busy(%rip)
	jnc .Luintr_entry_reserved
	ud2 /* Entry overlap with live UIF clear violates the entry invariant. */
.Luintr_entry_reserved:
	movq %rax, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_rax(%rip)
	movq %r10, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_r10(%rip)
	movq %r11, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_r11(%rip)
#ifdef CONFIG_FLUX_MPK
	movq 0x20(%rsp), %r11
	movq %r11, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_rsi(%rip)
	movq %rsi, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_pkru(%rip)
	movq %rsp, %r11
	movq %r11, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_hw_rsp(%rip)
#else
	movq %rsp, %gs:tls_pcpu+PCPU_uintr_entry+UENTRY_hw_rsp(%rip)
#endif
	leaq tls_pcpu+PCPU_uintr_entry(%rip), %r10
#ifdef CONFIG_FLUX_MPK
	movq %gs:UENTRY_rsi(%r10), %rsi
#endif
.endm

.macro UINTR_RELEASE_ENTRY
	lock btrq $0, %gs:tls_pcpu+PCPU_uintr_entry_busy(%rip)
.endm

/*
 * Select the entry before either path changes the syscall ABI or switches
 * PKRU.  These syscalls need the complete pt_regs frame built by the slow
 * path; all other calls use the same fast helper as the Flux libc gate.
 *
 * rax: syscall number
 * rdi, rsi, rdx, r10, r8, r9: args 1~6
 */
.pushsection .text, "ax"
SYM_FUNC_START(flux_syscall_dispatch)
	cmpq $__NR_rt_sigreturn, %rax
	je flux_syscall
	cmpq $__NR_clone, %rax
	je flux_syscall
	cmpq $__NR_fork, %rax
	je flux_syscall
	cmpq $__NR_vfork, %rax
	je flux_syscall
	cmpq $__NR_clone3, %rax
	je flux_syscall
	jmp flux_syscall_rewrite_fast
SYM_FUNC_END(flux_syscall_dispatch)
.popsection

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
 * Entry for a rewritten rt_sigreturn restorer that tail-jumps here without a
 * CALL.  A host signal restorer reaches this entry with kernel PKRU and its
 * Linux frame still live on the pkey-0 host signal stack.  Preserve that RSP
 * and invoke the real host rt_sigreturn so Linux consumes its frame and the
 * kmod hook completes the captured-UIF transition.
 *
 * A Flux signal restorer reaches this entry with application PKRU.  Its RSP
 * is eight bytes above the Flux struct rt_sigframe because the handler's RET
 * has consumed pretcode.  Save RSP-8 as the logical Flux stack pointer
 * expected by syscall_prepare_regs(), reconstructing the normal CALL layout
 * entirely on the protected kernel stack.
 */
.pushsection .text, "ax"
SYM_FUNC_START(flux_syscall_sigreturn_nostack)
#ifdef CONFIG_FLUX_MPK
	xorl %ecx, %ecx
	.byte 0x0f, 0x01, 0xee /* rdpkru */
	testl %eax, %eax
	jz .Lflux_host_rt_sigreturn
	FLUX_MPK_ENTER_SYSCALL
#endif
	PER_CPU_VAR tls_pcpu PCPU_stack_top %r11
	subq $0x8, %r11
	leaq -0x8(%rsp), %rax
	movq %rax, (%r11)
	movq %r11, %rsp

	pushfq
	subq $0x18, %rsp
	PUSH_ALL

	movq $__NR_rt_sigreturn, %rsi
	movq %rsp, %rdi
	andq $-16, %rsp
	call syscall_entry_slow
	NORET
#ifdef CONFIG_FLUX_MPK
.Lflux_host_rt_sigreturn:
	movl $__NR_rt_sigreturn, %eax
	syscall
	ud2
#endif
SYM_FUNC_END(flux_syscall_sigreturn_nostack)
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
	rdfsbase %r10
	movq %r10, 0x80(%rsp) /* stash host fsbase in regs->uirrv */
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
	.globl __flux_syscall_fast_end
__flux_syscall_fast_end:
SYM_FUNC_END(flux_syscall_fast)
.popsection

/*
 * Fast entry for a rewritten syscall.  Unlike flux_syscall_fast, this accepts
 * the Linux syscall ABI and restores every register that SYSCALL preserves.
 *
 * rax: syscall number
 * rdi, rsi, rdx, r10, r8, r9: args 1~6
 */
.pushsection .text, "ax"
SYM_FUNC_START(flux_syscall_rewrite_fast)
#ifdef CONFIG_FLUX_MPK
	FLUX_MPK_ENTER_SYSCALL
#endif
	/* retrieve the kernel stack */
	PER_CPU_VAR tls_pcpu PCPU_stack_top %r11

	/* save user rsp */
	subq $0x8, %r11
	movq %rsp, (%r11)

	/* switch to the kernel stack and build pt_regs */
	movq %r11, %rsp
	pushfq
	subq $0x18, %rsp
	PUSH_ALL

	/* Convert the Linux syscall ABI to syscall_entry_fast's C ABI. */
	movq %r9, %r11
	movq %r8, %r9
	movq %r10, %r8
	movq %rdx, %rcx
	movq %rsi, %rdx
	movq %rdi, %rsi
	movq %rax, %rdi

	/* Keep the 160-byte pt_regs frame 16-byte aligned. */
	subq $0x8, %rsp
	pushq %r11
	call syscall_entry_fast

	/* Restore the raw-syscall register contract with RAX as the result. */
	addq $0x10, %rsp
	movq %rax, REG_rax(%rsp)
	POP_ALL
	addq $0x20, %rsp /* skip orig_ax, uirrv, ip and flags */
	movq (%rsp), %rsp
	subq $0x8, %rsp
	btrq $63, (%rsp)

#ifdef CONFIG_FLUX_MPK
	FLUX_MPK_EXIT_REWRITTEN_SYSCALL
#endif
	ret
	.globl __flux_syscall_rewrite_fast_end
__flux_syscall_rewrite_fast_end:
SYM_FUNC_END(flux_syscall_rewrite_fast)
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

.pushsection .text, "ax"
/*
 * Host rt_sigreturn has restored the application context with UIF clear.
 * signal.c has saved the four scratch GPRs above a synthetic tail on the
 * protected task UINTR stack, followed by the interrupted PKRU. Open PKRU
 * before touching that stack, then
 * join the normal entry with every interrupted register preserved.
 */
SYM_CODE_START(flux_uintr_signal_stack)
#ifdef CONFIG_FLUX_MPK
	xorl %eax, %eax
	xorl %ecx, %ecx
	xorl %edx, %edx
	.byte 0x0f, 0x01, 0xef /* wrpkru */
	testl %eax, %eax
	jne .Lflux_mpk_bad_pkru
	movq 0x38(%rsp), %rdx
	movq 0x30(%rsp), %rcx
	movq 0x28(%rsp), %rax
	movl 0x40(%rsp), %esi
#endif
	jmp .Luintr_save_entry
SYM_CODE_END(flux_uintr_signal_stack)

SYM_CODE_START(flux_uintr_handler)
#ifdef CONFIG_FLUX_MPK
	FLUX_MPK_ENTER_UINTR_SCRATCH
#endif

	/*
	 * The hardware return frame can end exactly at the bottom of a mapped
	 * Flux stack page.  Do not push even one word there.  Atomically reserve
	 * the per-CPU scratch without clobbering any interrupted register,
	 * then save the three scratch GPRs and the hardware-frame pointer there.
	 * Real and synthetic UINTR entry both start with live UIF clear, so the
	 * scratch is released before any later handler work can admit nesting.
	 */
.Luintr_save_entry:
	UINTR_SAVE_ENTRY

	/*
	 * Save the interrupted runtime FSBASE before the C handler can schedule
	 * or switch Flux tasks.  r10 now addresses this entry's reserved scratch;
	 * all following scratch accesses use %gs and remain isolated from nesting.
	 */
	rdfsbase %rax
	movq %rax, %gs:UENTRY_fsbase(%r10)

	PER_CPU_DIRECT_LOAD tls_pcpu PCPU_uintr_stack_top %r11

.Luintr_select_stack:
	movq %gs:UENTRY_hw_rsp(%r10), %rax
	subq $(THREAD_SIZE / 2), %r11
	cmpq %r11, %rax
	jb .Luintr_use_stack_top
	addq $(THREAD_SIZE / 2), %r11
	cmpq %r11, %rax
	jb .Luintr_build_regs

.Luintr_use_stack_top:
	PER_CPU_DIRECT_LOAD tls_pcpu PCPU_uintr_stack_top %r11
	movq %r11, %rsp
	jmp .Luintr_have_stack

.Luintr_build_regs:
	movq %rax, %rsp
	jmp .Luintr_have_stack

.Luintr_have_stack:
	subq $0x28, %rsp /* orig_ax + copied uirrv/ip/flags/sp */
	PUSH_ALL

	/* Restore the registers we used before PUSH_ALL. */
	movq %gs:UENTRY_r11(%r10), %r11
	movq %r11, REG_r11(%rsp)
	movq %gs:UENTRY_r10(%r10), %r11
	movq %r11, REG_r10(%rsp)
	movq %gs:UENTRY_rax(%r10), %r11
	movq %r11, REG_ax(%rsp)

	/* Stash this frame's interrupted FSBASE in pt_regs.orig_ax. */
	movq %gs:UENTRY_fsbase(%r10), %r11
	movq %r11, REG_orig_ax(%rsp)

	/* Copy the hardware UINTR return frame into the synthetic pt_regs. */
	movq %gs:UENTRY_hw_rsp(%r10), %r11
	movq 0x00(%r11), %r11
	movq %r11, REG_uirrv(%rsp)
	movq %gs:UENTRY_hw_rsp(%r10), %r11
	movq 0x08(%r11), %r11
	movq %r11, REG_ip(%rsp)
	movq %gs:UENTRY_hw_rsp(%r10), %r11
	movq 0x10(%r11), %r11
	movq %r11, REG_flags(%rsp)
	movq %gs:UENTRY_hw_rsp(%r10), %r11
	movq 0x18(%r11), %r11
	movq %r11, REG_rsp(%rsp)
#ifdef CONFIG_FLUX_MPK
	movl %gs:UENTRY_pkru(%r10), %esi
#endif
	UINTR_RELEASE_ENTRY

	/*
	 * Each asynchronous entry owns an aligned, full-size xstate image below
	 * its pt_regs. Keep it outside the C frame and above the handler stack so
	 * nested entries and a later task switch cannot reuse a live image.
	 */
	movq %rsp, %rdi
	andq $-64, %rsp
	subq $PAGE_SIZE, %rsp
#ifdef CONFIG_FLUX_MPK
	movq %rsp, %rdx
#else
	movq %rsp, %rsi
#endif
	/* UINTR clears TF/RF, but C string operations also require DF clear. */
	cld
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
#ifdef CONFIG_FLUX_MPK
	testl %esi, %esi
	jz .Luintr_ret_kernel
	cmpl $FLUX_MPK_APP_PKRU, %esi
	jne .Lflux_mpk_bad_pkru
	/*
	 * The shared-pkey slot lets us restore the WRPKRU operands after lowering
	 * PKRU and return with one UIRET, without writing the interrupted stack.
	 */
	FLUX_MPK_UIRET_TO_USER %rdi %rax %rcx
.Luintr_ret_kernel:
#endif
	movq %rdi, %rsp
	POP_ALL
	add $0x10, %rsp /* skip orig_ax and uirrv */
	uiret
SYM_CODE_END(__ret_from_uintr)
.popsection

/*
 * Complete the hardware UINTR before generic user-exit work can schedule.
 * The one UIRET enables UIF into a continuation which still has kernel
 * PKRU and a protected stack.  Generic exit work may
 * then schedule safely, and syscall_ret_to_user restores the interrupted
 * context without reading or changing UIF.  A pending nested UINTR at the
 * continuation boundary therefore also sees kernel PKRU and a writable
 * protected entry stack.
 *
 * rdi: protected user-exit context with the register and fault snapshots
 */
.pushsection .text, "ax"
SYM_CODE_START(__ret_from_uintr_user_exit)
	/* Keep the continuation stack below its live context and SysV aligned. */
	movq %rdi, %rax
	andq $-16, %rax
	subq $0x8, %rax
	movq $0, (%rax)

	/*
	 * Build a private UIRET frame without overwriting the saved context.
	 * Enter C with DF/TF clear. Bit 1 also enables UIF on flexible-UIRET
	 * CPUs; older CPUs enable UIF unconditionally. The original application
	 * flags remain in pt_regs for the final application return.
	 */
	subq $0x18, %rsp
	leaq uintr_user_exit(%rip), %rcx
	movq %rcx, 0x00(%rsp)
	movq $X86_EFLAGS_FIXED, 0x08(%rsp)
	movq %rax, 0x10(%rsp)
	uiret
SYM_CODE_END(__ret_from_uintr_user_exit)
.popsection

/*
 * Complete one synthetic UINTR into the sleepable kernel-fault continuation.
 * RDI points to its context in the caller's still-live protected stack frame.
 */
.pushsection .text, "ax"
SYM_CODE_START(__ret_from_uintr_kernel_fault)
	leaq -0x10(%rsp), %rax
	movq $0, (%rax)
	subq $0x28, %rsp
	leaq uintr_kernel_fault_exit(%rip), %rcx
	movq %rcx, 0x00(%rsp)
	movq $X86_EFLAGS_FIXED, 0x08(%rsp)
	movq %rax, 0x10(%rsp)
	uiret
SYM_CODE_END(__ret_from_uintr_kernel_fault)

/*
 * Resume the interrupted kernel instruction with all GPRs, RFLAGS and RSP.
 * The three words occupy the reserved return area below the SysV red zone.
 * Hardware delivery and MPK entry scratch both stay below that area, even
 * when interrupted after RSP has been restored but before the final JMP.
 * No per-CPU slot is shared.
 * PKRU remains kernel; the one UIRET already restored UIF.
 */
SYM_CODE_START(__ret_from_kernel_fault)
	movq REG_rsp(%rdi), %rax
	movq REG_flags(%rdi), %rcx
	movq %rcx, -152(%rax)
	movq %rax, -144(%rax)
	movq REG_ip(%rdi), %rcx
	movq %rcx, -136(%rax)
	movq %rdi, %rsp
	POP_ALL
	movq 0x20(%rsp), %rsp
	leaq -152(%rsp), %rsp
	popfq
	popq %rsp
	jmp *-136(%rsp)
SYM_CODE_END(__ret_from_kernel_fault)
.popsection
