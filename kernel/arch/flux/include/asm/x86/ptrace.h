/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PTRACE_H
#define _ASM_X86_PTRACE_H

#include <asm/thread_info.h>

#ifndef __ASSEMBLY__

struct pt_regs {
	/*
	 * C ABI says these regs are callee-preserved. They aren't saved on kernel entry
	 * unless syscall needs a complete, fully filled "struct pt_regs".
	 */
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 bp;
	u64 bx;
	/* These regs are callee-clobbered. Always saved on kernel entry. */
	u64 r11;
	u64 r10;
	u64 r9;
	u64 r8;
	u64 cx;
	u64 dx;
	u64 si;
	u64 di;
	u64 ax;
	u64 orig_ax; /* syscall */
	u64 uirrv; /* saved on a User-Interrupt Delivery */
	/* Return frame for uiret */
	u64 ip;
	union {
		u64 flags;
		struct {
			u32 eflags;
			u32 : 1; /* RFLAGS bit 32 is not hardware UIF. */
			u32 umode : 1;
		};
	};
	u64 sp;
	/* top of stack page */
};

struct task_struct;

extern unsigned long profile_pc(struct pt_regs *regs);

static inline unsigned long regs_return_value(struct pt_regs *regs)
{
	return regs->ax;
}

static inline void regs_set_return_value(struct pt_regs *regs, unsigned long rc)
{
	regs->ax = rc;
}

#define current_user_stack_pointer() current_pt_regs()->sp
#define compat_user_stack_pointer() current_pt_regs()->sp

static inline unsigned long kernel_stack_pointer(struct pt_regs *regs)
{
	return regs->sp;
}

static inline unsigned long instruction_pointer(struct pt_regs *regs)
{
	return regs->ip;
}

static inline void instruction_pointer_set(struct pt_regs *regs,
					   unsigned long val)
{
	regs->ip = val;
}

static inline unsigned long frame_pointer(struct pt_regs *regs)
{
	return regs->bp;
}

static inline unsigned long user_stack_pointer(struct pt_regs *regs)
{
	return regs->sp;
}

static inline void user_stack_pointer_set(struct pt_regs *regs,
					  unsigned long val)
{
	regs->sp = val;
}

static __always_inline bool regs_irqs_disabled(struct pt_regs *regs)
{
	/*
	 * This describes the interrupted state, not the live UIF.  Real UINTR
	 * and synthetic signal injection enter only for captured prior UIF=1;
	 * captured UIF=0 leaves the work pending instead.
	 */
	(void)regs;
	return false;
}

#define user_mode(regs) (regs->umode)
#define kernel_mode(regs) (!regs->umode)

#define MAX_REG_OFFSET (offsetof(struct pt_regs, sp))
/**
 * regs_get_register() - get register value from its offset
 * @regs:	pt_regs from which register value is gotten.
 * @offset:	offset number of the register.
 *
 * regs_get_register returns the value of a register. The @offset is the
 * offset of the register in struct pt_regs address which specified by @regs.
 * If @offset is bigger than MAX_REG_OFFSET, this returns 0.
 */
static inline unsigned long regs_get_register(struct pt_regs *regs,
					      unsigned int offset)
{
	if (unlikely(offset > MAX_REG_OFFSET))
		return 0;
	return *(unsigned long *)((unsigned long)regs + offset);
}

/**
 * regs_within_kernel_stack() - check the address in the stack
 * @regs:	pt_regs which contains kernel stack pointer.
 * @addr:	address which is checked.
 *
 * regs_within_kernel_stack() checks @addr is within the kernel stack page(s).
 * If @addr is within the kernel stack, it returns true. If not, returns false.
 */
static inline int regs_within_kernel_stack(struct pt_regs *regs,
					   unsigned long addr)
{
	return ((addr & ~(THREAD_SIZE - 1)) == (regs->sp & ~(THREAD_SIZE - 1)));
}

/**
 * regs_get_kernel_stack_nth_addr() - get the address of the Nth entry on stack
 * @regs:	pt_regs which contains kernel stack pointer.
 * @n:		stack entry number.
 *
 * regs_get_kernel_stack_nth() returns the address of the @n th entry of the
 * kernel stack which is specified by @regs. If the @n th entry is NOT in
 * the kernel stack, this returns NULL.
 */
static inline unsigned long *
regs_get_kernel_stack_nth_addr(struct pt_regs *regs, unsigned int n)
{
	unsigned long *addr = (unsigned long *)regs->sp;

	addr += n;
	if (regs_within_kernel_stack(regs, (unsigned long)addr))
		return addr;
	else
		return NULL;
}

/* To avoid include hell, we can't include uaccess.h */
extern long copy_from_kernel_nofault(void *dst, const void *src, size_t size);

/**
 * regs_get_kernel_stack_nth() - get Nth entry of the stack
 * @regs:	pt_regs which contains kernel stack pointer.
 * @n:		stack entry number.
 *
 * regs_get_kernel_stack_nth() returns @n th entry of the kernel stack which
 * is specified by @regs. If the @n th entry is NOT in the kernel stack
 * this returns 0.
 */
static inline unsigned long regs_get_kernel_stack_nth(struct pt_regs *regs,
						      unsigned int n)
{
	unsigned long *addr;
	unsigned long val;
	long ret;

	addr = regs_get_kernel_stack_nth_addr(regs, n);
	if (addr) {
		ret = copy_from_kernel_nofault(&val, addr, sizeof(val));
		if (!ret)
			return val;
	}
	return 0;
}

/**
 * regs_get_kernel_argument() - get Nth function argument in kernel
 * @regs:	pt_regs of that context
 * @n:		function argument number (start from 0)
 *
 * regs_get_argument() returns @n th argument of the function call.
 * Note that this chooses most probably assignment, in some case
 * it can be incorrect.
 * This is expected to be called from kprobes or ftrace with regs
 * where the top of stack is the return address.
 */
static inline unsigned long regs_get_kernel_argument(struct pt_regs *regs,
						     unsigned int n)
{
	static const unsigned int argument_offs[] = {
		offsetof(struct pt_regs, di), offsetof(struct pt_regs, si),
		offsetof(struct pt_regs, dx), offsetof(struct pt_regs, cx),
		offsetof(struct pt_regs, r8), offsetof(struct pt_regs, r9),
#define NR_REG_ARGUMENTS 6
	};

	if (n >= NR_REG_ARGUMENTS) {
		n -= NR_REG_ARGUMENTS - 1;
		return regs_get_kernel_stack_nth(regs, n);
	} else
		return regs_get_register(regs, argument_offs[n]);
}

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_X86_PTRACE_H */
