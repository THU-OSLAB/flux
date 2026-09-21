// SPDX-License-Identifier: GPL-2.0
/* Linux task permissions, with real port I/O confined to the fault path. */
#include <linux/bitmap.h>
#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <asm/extable.h>
#include <asm/insn.h>
#include <asm/ioport.h>
#include <asm/mpk_uaccess.h>
#include <asm/processor.h>
#include <asm/signal.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>
#include <asm/x86/processor-flags.h>
#include <uapi/asm-generic/ucontext.h>

#define IO_BITMAP_BITS 65536UL
#define IO_BITMAP_LONGS (IO_BITMAP_BITS / BITS_PER_LONG)

/* Same permission representation as arch/x86/include/asm/io_bitmap.h. */
struct io_bitmap {
	u64 sequence;
	refcount_t refcnt;
	unsigned int max;
	unsigned long bitmap[IO_BITMAP_LONGS];
};

/*
 * Linux 6.6 implements iopl through a bitmap, not architectural IOPL bits.
 * Borrow that native facility only while executing trusted Flux C code.
 * Restore it before allowing any Flux task switch or application return.
 * No per-worker permissions or synchronization enter the scheduler path.
 */
static long flux_io_backend_ready(void)
{
	long ret;

	preempt_disable();
	ret = host_syscall(__NR_iopl, 3);
	if (!ret)
		BUG_ON(host_syscall(__NR_iopl, 0));
	preempt_enable_no_resched();
	return ret;
}

#include "ioport-core.inc"

struct flux_io_result {
	unsigned long value;
	unsigned long remaining;
	unsigned long address;
	unsigned long error;
	unsigned long trap;
};

/* Preserve live string registers and the exact hardware fault address. */
static long flux_io_transfer(unsigned int port, unsigned int bytes, bool input,
			     bool string, unsigned long pointer, bool backward,
			     struct flux_io_result *result)
{
	register unsigned long fault_address asm("r8");
	register unsigned long fault_error asm("r9");
	register unsigned long fault_trap asm("r10");
	unsigned long value = result->value, count = 1;
	long ret;

	preempt_disable();
	ret = host_syscall(__NR_iopl, 3);
	if (ret)
		goto out;
	pagefault_disable();
	fault_address = fault_error = fault_trap = 0;
#define IO_FAULT _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_FLUX_IOPORT)
#define IO_ERRORS "+r" (fault_address), "+r" (fault_error), "+r" (fault_trap)
#define IO_IN(opsize, regsize) asm volatile("1: in" opsize " %w[port], %" regsize "[value]\n2:\n" \
	IO_FAULT : [value] "+a" (value), IO_ERRORS : [port] "d" (port) : "memory")
#define IO_OUT(opsize, regsize) asm volatile("1: out" opsize " %" regsize "[value], %w[port]\n2:\n" \
	IO_FAULT : [value] "+a" (value), IO_ERRORS : [port] "d" (port) : "memory")
#define IO_STRING(op, constraint) asm volatile( \
	"cld\n\ttest %[backward], %[backward]\n\tjz 1f\n\tstd\n" \
	"1: rep " op "\n2: cld\n" IO_FAULT \
	: constraint (pointer), "+c" (count), IO_ERRORS \
	: "d" (port), [backward] "r" ((unsigned long)backward) : "memory", "cc")
	if (string) {
		if (input) {
			switch (bytes) {
			case 1: IO_STRING("insb", "+D"); break;
			case 2: IO_STRING("insw", "+D"); break;
			default: IO_STRING("insl", "+D"); break;
			}
		} else {
			switch (bytes) {
			case 1: IO_STRING("outsb", "+S"); break;
			case 2: IO_STRING("outsw", "+S"); break;
			default: IO_STRING("outsl", "+S"); break;
			}
		}
	} else if (input) {
		switch (bytes) {
		case 1: IO_IN("b", "b"); break;
		case 2: IO_IN("w", "w"); break;
		default: IO_IN("l", "k"); break;
		}
	} else {
		switch (bytes) {
		case 1: IO_OUT("b", "b"); break;
		case 2: IO_OUT("w", "w"); break;
		default: IO_OUT("l", "k"); break;
		}
	}
#undef IO_STRING
#undef IO_OUT
#undef IO_IN
#undef IO_ERRORS
#undef IO_FAULT
	result->value = value;
	result->remaining = count;
	result->address = fault_address;
	result->error = fault_error;
	result->trap = fault_trap;
	pagefault_enable();
	BUG_ON(host_syscall(__NR_iopl, 0));
out:
	preempt_enable_no_resched();
	return ret;
}

static bool io_prefix(const struct insn *insn, u8 prefix)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(insn->prefixes.bytes); i++)
		if (insn->prefixes.bytes[i] == prefix)
			return true;
	return false;
}

bool flux_io_is_insn(const struct insn *insn)
{
	u8 op = insn->opcode.bytes[0];

	return insn->opcode.nbytes == 1 && !io_prefix(insn, 0xf0) &&
	       ((op >= 0xe4 && op <= 0xe7) || (op >= 0xec && op <= 0xef) ||
		(op >= 0x6c && op <= 0x6f));
}

static void io_general_fault(int *sig, siginfo_t *si, struct sigcontext *sc)
{
	*sig = si->si_signo = SIGSEGV;
	si->si_code = SI_KERNEL;
	si->si_addr = NULL;
	sc->trapno = 13;
	sc->err = 0;
}

static void io_memory_fault(int *sig, siginfo_t *si, struct sigcontext *sc,
			    unsigned long address, unsigned long error, int code)
{
	*sig = si->si_signo = SIGSEGV;
	si->si_code = code;
	si->si_addr = (void *)address;
	sc->trapno = 14;
	sc->err = error;
	sc->cr2 = address;
}

/* The caller holds mmap_lock; check logical permissions before native uaccess. */
static bool io_memory_access(unsigned long address, unsigned int bytes,
			     bool write, unsigned long *bad, int *code)
{
	unsigned long end, pos;
	struct vm_area_struct *vma;

	*bad = address;
	*code = SEGV_MAPERR;
	if (check_add_overflow(address, bytes, &end) ||
	    !flux_mpk_uaccess_range_ok((void __user *)address, bytes))
		return false;
	for (pos = address; pos < end; pos = min(end, vma->vm_end)) {
		*bad = pos;
		vma = find_vma(current->mm, pos);
		if (!vma || pos < vma->vm_start)
			return false;
		if (!(vma->vm_flags & (write ? VM_WRITE :
					       (VM_READ | VM_WRITE | VM_EXEC)))) {
			*code = SEGV_ACCERR;
			return false;
		}
	}
	return true;
}

bool flux_io_handle(int *sig, siginfo_t *si, void *context,
		    const struct insn *insn)
{
	struct sigcontext *sc = &((struct ucontext *)context)->uc_mcontext;
	struct io_bitmap *bitmap = current->thread.io_bitmap;
	u8 op = insn->opcode.bytes[0];
	bool string = op >= 0x6c && op <= 0x6f;
	bool input = !(op & 2);
	bool repeat = string && (io_prefix(insn, 0xf2) || io_prefix(insn, 0xf3));
	bool address32 = insn->addr_bytes == 4;
	bool backward = sc->flags & X86_EFLAGS_DF;
	unsigned int bytes = !(op & 1) ? 1 : io_prefix(insn, 0x66) ? 2 : 4;
	unsigned int port = op >= 0xe4 && op <= 0xe7 ?
			    (u8)insn->immediate.value : (u16)sc->dx;
	unsigned int i, budget = 64;

	if (port + bytes > IO_BITMAP_BITS)
		goto denied;
	if (current->thread.iopl_emul != 3) {
		if (!bitmap)
			goto denied;
		for (i = port; i < port + bytes; i++)
			if (test_bit(i, bitmap->bitmap))
				goto denied;
	}
	/* MPK application GS is reserved by the existing Flux application ABI. */
	if (string && !input && io_prefix(insn, 0x65))
		goto denied;

	while (budget--) {
		struct flux_io_result result = { .value = sc->ax };
		unsigned long pointer = input ? sc->di : sc->si;
		unsigned long bad;
		int code;
		long ret;

		if (repeat && !(address32 ? (u32)sc->cx : sc->cx))
			break;
		if (string) {
			pointer = address32 ? (u32)pointer : pointer;
			if (!input && io_prefix(insn, 0x64) &&
			    check_add_overflow(pointer, current->thread.fsbase,
					       &pointer))
				goto denied;
			if (!mmap_read_trylock(current->mm))
				return true;
			if (!io_memory_access(pointer, bytes, input, &bad, &code)) {
				mmap_read_unlock(current->mm);
				io_memory_fault(sig, si, sc, bad, input ? 6 : 4, code);
				return false;
			}
		}
		ret = flux_io_transfer(port, bytes, input, string, pointer,
				       backward, &result);
		if (string)
			mmap_read_unlock(current->mm);
		if (ret)
			goto denied;
		if (result.trap) {
			if ((result.trap & ~(1UL << 63)) != 14)
				goto denied;
			io_memory_fault(sig, si, sc, result.address,
					result.error | 4,
					result.error & 1 ? SEGV_ACCERR : SEGV_MAPERR);
			return false;
		}
		if (string) {
			unsigned long next = (input ? sc->di : sc->si) +
					     (backward ? -(long)bytes : (long)bytes);

			if (input)
				sc->di = address32 ? (u32)next : next;
			else
				sc->si = address32 ? (u32)next : next;
			if (repeat)
				sc->cx = address32 ? (u32)(sc->cx - 1) : sc->cx - 1;
		} else if (input) {
			if (bytes == 4)
				sc->ax = (u32)result.value;
			else {
				unsigned long mask = (1UL << (8 * bytes)) - 1;

				sc->ax = (sc->ax & ~mask) | (result.value & mask);
			}
		}
		if (!repeat)
			break;
	}
	if (!repeat || !(address32 ? (u32)sc->cx : sc->cx))
		sc->ip += insn->length;
	return true;
denied:
	io_general_fault(sig, si, sc);
	return false;
}

bool flux_io_signal(int *sig, siginfo_t *si, void *context)
{
	struct sigcontext *sc = &((struct ucontext *)context)->uc_mcontext;
	u8 bytes[MAX_INSN_SIZE];
	struct insn insn;
	unsigned long remaining;

	pagefault_disable();
	remaining = copy_from_user(bytes, (void __user *)sc->ip, sizeof(bytes));
	pagefault_enable();
	if (insn_decode(&insn, bytes, sizeof(bytes) - remaining, INSN_MODE_64) ||
	    !flux_io_is_insn(&insn))
		return false;
	return flux_io_handle(sig, si, context, &insn);
}
