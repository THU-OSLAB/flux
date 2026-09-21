// SPDX-License-Identifier: GPL-2.0
/* Shared and writable executable mappings use task-owned instruction slots. */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/insn.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/signal.h>
#include <asm/xol.h>
#include <asm/ioport.h>
#include <asm/x86/processor-flags.h>
#include <uapi/asm-generic/ucontext.h>

/* Definitions belong to the same in-tree x86 source as the generated core. */
#include <asm/uprobes.h>

struct flux_xol_state {
	struct arch_uprobe_task autask;
	unsigned long vaddr;
	unsigned long xol_vaddr;
	struct arch_uprobe auprobe;
	bool pending;
	bool syscall;
};

/* Flux implements the 64-bit application ABI. */
static bool is_64bit_mm(struct mm_struct *mm) { return true; }
static bool user_64bit_mode(struct pt_regs *regs) { return true; }

#include "xol-core.inc"

static void flux_xol_load_regs(struct pt_regs *regs, struct sigcontext *sc)
{
	memset(regs, 0, sizeof(*regs));
#define REG(field) regs->field = sc->field
	REG(r8); REG(r9); REG(r10); REG(r11); REG(r12); REG(r13);
	REG(r14); REG(r15); REG(ax); REG(bx); REG(cx); REG(dx);
	REG(si); REG(di); REG(bp); REG(sp); REG(ip); REG(flags);
#undef REG
	regs->umode = 1;
	regs->orig_ax = -1;
}

static void flux_xol_store_regs(struct sigcontext *sc, struct pt_regs *regs)
{
#define REG(field) sc->field = regs->field
	REG(r8); REG(r9); REG(r10); REG(r11); REG(r12); REG(r13);
	REG(r14); REG(r15); REG(ax); REG(bx); REG(cx); REG(dx);
	REG(si); REG(di); REG(bp); REG(sp); REG(ip);
#undef REG
	/* Flux's software umode bit is not an architectural RFLAGS bit. */
	sc->flags = regs->eflags;
}

static void flux_xol_abort(struct flux_xol_state *state, struct pt_regs *regs)
{
	if (state->auprobe.ops->abort)
		state->auprobe.ops->abort(&state->auprobe, regs);
	regs->ip = state->vaddr;
	if (!state->autask.saved_tf)
		regs->flags &= ~X86_EFLAGS_TF;
	state->pending = false;
}

/* Return true when the application's own TF requests a visible debug trap. */
static bool flux_xol_post(struct flux_xol_state *state, struct pt_regs *regs)
{
	bool trace = state->autask.saved_tf;
	int ret = 0;

	pagefault_disable();
	if (state->auprobe.ops->post_xol)
		ret = state->auprobe.ops->post_xol(&state->auprobe, regs);
	pagefault_enable();
	if (ret) {
		regs->ip = state->vaddr;
		trace = false;
	}
	if (!state->autask.saved_tf)
		regs->flags &= ~X86_EFLAGS_TF;
	state->pending = false;
	return trace;
}

/* Only exceptional executable VMAs reach the instruction decoder. */
static bool flux_xol_vma(struct mm_struct *mm, unsigned long address)
{
	struct vm_area_struct *vma = find_vma(mm, address);

	return vma && address >= vma->vm_start &&
	       flux_xol_required(vma->vm_flags);
}

static void flux_xol_memory_fault(struct sigcontext *sc, siginfo_t *si,
				  unsigned long address, bool write)
{
	sc->cr2 = address;
	sc->err = write ? 6 : 20; /* user write / user instruction fetch */
	sc->trapno = 14;
	si->si_signo = SIGSEGV;
	si->si_code = SEGV_ACCERR;
	si->si_addr = (void *)address;
}

static bool flux_xol_instruction_allowed(const struct insn *insn)
{
	/* The existing MPK/GS/UINTR state remains owned by Flux entry code. */
	if (insn->opcode.nbytes == 2 && insn->opcode.bytes[0] == 0x0f) {
		u8 opcode = insn->opcode.bytes[1];
		u8 modrm = insn->modrm.bytes[0];

		if (opcode == 0x01 || opcode == 0xc7 ||
		    (opcode == 0xae &&
		     ((modrm & 0xc0) != 0xc0 || insn->prefixes.nbytes)))
			return false;
	}
	return true;
}

/*
 * This runs inside an authenticated host signal frame, with migration pinned.
 * Allocate/populate only after returning through the existing deferred fault
 * path. TF is set here, where host rt_sigreturn installs it atomically with IP;
 * setting it before Flux's ordinary JMP return would single-step that return.
 */
bool flux_xol_signal(int *sig, siginfo_t *si, void *context)
{
	struct sigcontext *sc = &((struct ucontext *)context)->uc_mcontext;
	struct flux_xol_state *state = current->thread.xol;
	struct pt_regs regs;
	struct insn insn;
	unsigned long remaining, available, limit;
	int ret;

	if (state && state->pending) {
		flux_xol_load_regs(&regs, sc);
		if (*sig == SIGTRAP && si->si_code == TRAP_TRACE) {
			bool trace = flux_xol_post(state, &regs);

			flux_xol_store_regs(sc, &regs);
			if (trace)
				si->si_addr = (void *)regs.ip;
			return !trace;
		}
		/* A signal may precede the slot instruction or follow completion. */
		if (regs.ip == state->xol_vaddr)
			flux_xol_abort(state, &regs);
		else
			flux_xol_post(state, &regs);
		flux_xol_store_regs(sc, &regs);
		if (*sig == SIGILL || *sig == SIGFPE || *sig == SIGTRAP)
			si->si_addr = (void *)regs.ip;
		return false;
	}
	if (*sig != SIGSEGV || !(sc->err & 16) ||
	    !current->mm || !state)
		return false;
	if (!mmap_read_trylock(current->mm))
		return false;
	if (!flux_xol_vma(current->mm, sc->ip)) {
		mmap_read_unlock(current->mm);
		return false;
	}
	mmap_read_unlock(current->mm);

	state->syscall = false;
	memset(&state->auprobe, 0, sizeof(state->auprobe));
	pagefault_disable();
	remaining = copy_from_user(state->auprobe.insn, (void __user *)sc->ip,
				   MAX_UINSN_BYTES);
	pagefault_enable();
	available = MAX_UINSN_BYTES - remaining;
	if (!available)
		return false;
	ret = insn_decode(&insn, state->auprobe.insn, available, INSN_MODE_64);
	if (ret) {
		if (remaining) {
			flux_xol_memory_fault(sc, si, sc->ip + available, false);
			return false;
		}
		goto illegal;
	}
	/* A readable neighbor is not necessarily executable. */
	if (!mmap_read_trylock(current->mm))
		return false;
	for (limit = sc->ip; limit < sc->ip + insn.length;) {
		struct vm_area_struct *vma = find_vma(current->mm, limit);

		if (!vma || limit < vma->vm_start || !(vma->vm_flags & VM_EXEC)) {
			mmap_read_unlock(current->mm);
			flux_xol_memory_fault(sc, si, limit, false);
			return false;
		}
		limit = vma->vm_end;
	}
	mmap_read_unlock(current->mm);
	flux_xol_load_regs(&regs, sc);
	state->vaddr = regs.ip;
	if (insn.length == 2 && insn.opcode.nbytes == 2 &&
	    insn.opcode.bytes[0] == 0x0f && insn.opcode.bytes[1] == 0x05) {
		state->syscall = true;
		return false;
	}
	if (insn.opcode.nbytes == 1 && insn.opcode.bytes[0] == 0x9c) {
		unsigned long flags = regs.eflags & ~(X86_EFLAGS_VM | X86_EFLAGS_RF);
		unsigned int bytes = insn.opnd_bytes == 2 ? 2 : 8;

		pagefault_disable();
		remaining = copy_to_user((void __user *)(regs.sp - bytes),
					 &flags, bytes);
		pagefault_enable();
		if (remaining) {
			flux_xol_memory_fault(sc, si, regs.sp - remaining, true);
			return false;
		}
		regs.sp -= bytes;
		regs.ip += insn.length;
		flux_xol_store_regs(sc, &regs);
		return true;
	}
	if (flux_io_is_insn(&insn))
		return flux_io_handle(sig, si, context, &insn);
	if (!flux_xol_instruction_allowed(&insn))
		goto illegal;
	ret = flux_xol_analyze_insn(&state->auprobe, current->mm, regs.ip);
	if (ret)
		goto illegal;
	pagefault_disable();
	ret = state->auprobe.ops->emulate &&
	      state->auprobe.ops->emulate(&state->auprobe, &regs);
	pagefault_enable();
	if (ret) {
		flux_xol_store_regs(sc, &regs);
		return true;
	}
	memcpy((void *)state->xol_vaddr, state->auprobe.ixol, MAX_UINSN_BYTES);
	if (state->auprobe.ops->pre_xol) {
		ret = state->auprobe.ops->pre_xol(&state->auprobe, &regs);
		if (ret)
			goto illegal;
	}
	state->autask.saved_tf = !!(regs.flags & X86_EFLAGS_TF);
	regs.flags |= X86_EFLAGS_TF;
	regs.ip = state->xol_vaddr;
	state->pending = true;
	flux_xol_store_regs(sc, &regs);
	return true;

illegal:
	*sig = SIGILL;
	si->si_signo = SIGILL;
	si->si_code = ILL_ILLOPN;
	si->si_addr = (void *)sc->ip;
	sc->trapno = 6;
	return false;
}

/* Deferred context: host rt_sigreturn and the synthetic UIRET have completed. */
void flux_xol_finish_fault(struct pt_regs *regs,
			   const struct flux_deferred_fault *fault)
{
	struct flux_xol_state *state = current->thread.xol;
	bool required;

	if (!(fault->error & 16))
		return;
	mmap_read_lock(current->mm);
	required = flux_xol_vma(current->mm, regs->ip);
	mmap_read_unlock(current->mm);
	if (!required)
		return;
	if (!state) {
		void *slot;

		state = kzalloc(sizeof(*state), GFP_KERNEL);
		if (!state)
			goto nomem;
		slot = __vmalloc_node_range(PAGE_SIZE, PAGE_SIZE, VMALLOC_START,
					    VMALLOC_END, GFP_KERNEL | __GFP_ZERO,
					    PAGE_KERNEL_EXEC, 0, NUMA_NO_NODE,
					    __builtin_return_address(0));
		if (!slot) {
			kfree(state);
			goto nomem;
		}
		state->xol_vaddr = (unsigned long)slot;
		current->thread.xol = state;
	}
	if (state->syscall && state->vaddr == regs->ip) {
		state->syscall = false;
		flux_xol_syscall(regs);
	}
	/* No TF here: the next NX fault enters through a real host signal frame. */
	return;
nomem:
	force_sig(SIGSEGV);
}

void flux_xol_interrupt(struct pt_regs *regs)
{
	struct flux_xol_state *state = current->thread.xol;

	if (!state || !state->pending)
		return;
	if (regs->ip == state->xol_vaddr)
		flux_xol_abort(state, regs);
	else if (flux_xol_post(state, regs))
		force_sig_fault(SIGTRAP, TRAP_TRACE, (void __user *)regs->ip);
}

void flux_xol_release(struct task_struct *task)
{
	struct flux_xol_state *state = task->thread.xol;

	task->thread.xol = NULL;
	if (!state)
		return;
	vfree((void *)state->xol_vaddr);
	kfree(state);
}
