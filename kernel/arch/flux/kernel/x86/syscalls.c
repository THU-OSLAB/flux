// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "sys: " fmt

#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/entry-common.h>
#include <linux/linkage.h>
#include <linux/mm.h>
#include <linux/capability.h>
#include <linux/sched/signal.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/nospec.h>
#include <linux/mman.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/overflow.h>

#include <linux/perf_event.h>
#include <asm/mmu.h>
#include <asm/xol.h>
#include <asm/syscalls.h>
#include <asm/host_dev.h>
#include <asm/mpk_uaccess.h>
#include <asm/smp.h>
#include <asm/x86/current.h>
#include <asm/x86/syscall.h>
#include <asm/x86/processor.h>
#include <asm/x86/processor-flags.h>
#include <uapi/asm/flux_ops.h>
#include <uapi/asm/mpk.h>
#include <uapi/asm/rewrite.h>

typedef long (*syscall_ptr_t)(long a1, ...);

#define FLUX_IOPORT_COUNT 65536UL

/*
 * libopcodes has process-global decoder state. Keep only the analysis call
 * global, while the surrounding ELF load and address-space mutations remain
 * per-mm. This is a Flux kernel mutex, so a UINTR task switch can schedule
 * the owner instead of blocking the host worker behind a userspace lock.
 */
static DEFINE_MUTEX(flux_rewrite_decoder_lock);

void flux_rewrite_state_lock(void)
{
	/* The boot VDSO is rewritten before a user mm exists. */
	struct mm_struct *mm = current->mm ?: &init_mm;

	mutex_lock(&mm->context.host_rewrite_state_mutex);
	/* Scheduling restores task TLS; the caller resumes native runtime C. */
	wrfsbase(flux_host_fsbase());
}

void flux_rewrite_state_unlock(void)
{
	struct mm_struct *mm = current->mm ?: &init_mm;

	mutex_unlock(&mm->context.host_rewrite_state_mutex);
	wrfsbase(flux_host_fsbase());
}

static long flux_rewrite_exec_serialized(void *addr, unsigned long len)
{
	long ret;

	mutex_lock(&flux_rewrite_decoder_lock);
	ret = flux_ops_rewrite_exec(addr, len);
	mutex_unlock(&flux_rewrite_decoder_lock);
	return ret;
}

#define FLUX_MAP_FIXED_FLAGS (MAP_FIXED | MAP_FIXED_NOREPLACE)

static __always_inline bool flux_rewrite_reserved_range(unsigned long start,
						 unsigned long len)
{
	if (!len)
		return false;
	if (start < PAGE_SIZE)
		return true;
	if (start >= FLUX_MPK_RETURN_ADDR + FLUX_MPK_RETURN_AREA_SIZE)
		return false;

	return start >= FLUX_RODATA_ADDR || len > FLUX_RODATA_ADDR - start;
}

static __always_inline bool flux_wx_prot(unsigned long prot)
{
	return (prot & (PROT_WRITE | PROT_EXEC)) ==
	       (PROT_WRITE | PROT_EXEC);
}

static long flux_invalidate_exec_range(unsigned long start,
				       unsigned long len, bool restore)
{
	if (!len)
		return 0;
	if (len > ULONG_MAX - (PAGE_SIZE - 1))
		return -EINVAL;
	return flux_ops_invalidate_exec((void *)start, PAGE_ALIGN(len),
					       restore);
}

static bool flux_user_range_has_flags(unsigned long start, unsigned long len,
				       vm_flags_t flags, bool direct_exec)
{
	struct vm_area_struct *vma;
	unsigned long end;
	bool shared = false;

	if (!current->mm || !len || check_add_overflow(start, len, &end))
		return false;

	mmap_read_lock(current->mm);
	for (vma = find_vma(current->mm, start);
	     vma && vma->vm_start < end;
	     vma = find_vma(current->mm, vma->vm_end)) {
		if (vma->vm_end > start && (vma->vm_flags & flags) &&
		    (!direct_exec || !flux_xol_required(vma->vm_flags))) {
			shared = true;
			break;
		}
	}
	mmap_read_unlock(current->mm);
	return shared;
}

static bool flux_user_range_has_direct_exec(unsigned long start, unsigned long len)
{
	return flux_user_range_has_flags(start, len, VM_EXEC, true);
}

/*
 * The PTE lock orders this publication against the following slow GUP walk.
 * Reclaim which passed the check first finishes before GUP installs the PTE;
 * reclaim which follows GUP sees the active range and leaves that PTE intact.
 * The range stays published through final protection or failure rollback.
 * Neither the mmap lock nor this small publication lock is held across faults,
 * decoder work, or host alias operations; userfaultfd handlers remain runnable.
 */
static void flux_rewrite_range_publish(unsigned long start, unsigned long end)
{
	mm_context_t *ctx = &current->mm->context;
	unsigned long flags;

	lockdep_assert_held(&ctx->host_rewrite_mutex);
	write_seqlock_irqsave(&ctx->host_rewrite_range_lock, flags);
	ctx->host_rewrite_start = start;
	ctx->host_rewrite_end = end;
	write_sequnlock_irqrestore(&ctx->host_rewrite_range_lock, flags);
}

bool flux_rewrite_range_busy(struct mm_struct *mm, unsigned long start,
			     unsigned long end)
{
	mm_context_t *ctx = &mm->context;
	unsigned long first, last;
	unsigned int seq;

	do {
		seq = read_seqbegin(&ctx->host_rewrite_range_lock);
		first = ctx->host_rewrite_start;
		last = ctx->host_rewrite_end;
	} while (read_seqretry(&ctx->host_rewrite_range_lock, seq));

	return start < last && end > first;
}

/* Rewriting must fault pages in even when the VMA has VM_LOCKONFAULT. */
static long flux_populate_rewrite_range(unsigned long start, unsigned long len)
{
	unsigned long nr_pages = len >> PAGE_SHIFT;
	long ret;

	while (nr_pages) {
		/* The transaction has already granted temporary write access. */
		ret = get_user_pages_unlocked(start, nr_pages, NULL, FOLL_WRITE);
		if (ret <= 0)
			return ret ? ret : -EFAULT;
		start += ret << PAGE_SHIFT;
		nr_pages -= ret;
	}
	return 0;
}

static long flux_protect_user_range(unsigned long start, unsigned long len,
				     unsigned long final_prot)
{
	unsigned long rewrite_len;
	unsigned int read_implies_exec;
	unsigned long rewrite_prot =
		(final_prot | PROT_READ | PROT_WRITE) & ~PROT_EXEC;
	unsigned long failure_prot = final_prot & ~(PROT_WRITE | PROT_EXEC);
	long ret;
	bool range_published = false;
#ifdef CONFIG_FLUX_MPK
	bool kernel_pkey = false;
#endif

	if (!len)
		return 0;
	if (len > ULONG_MAX - (PAGE_SIZE - 1))
		return -EINVAL;
	rewrite_len = PAGE_ALIGN(len);

	/*
	 * A range which is neither executable now nor becoming executable has no
	 * rewrite state to create or invalidate.  Keep its protection transition
	 * entirely in the LibOS mm; flush_tlb_range() revokes the old host
	 * aliases so subsequent accesses use the final Linux PTE permissions. Do not temporarily rekey
	 * ordinary data aliases to the kernel pkey: that intermediate state
	 * is visible to another userspace thread racing mprotect().
	 */
	if (!(final_prot & PROT_EXEC) &&
	    !flux_user_range_has_direct_exec(start, len))
		return sys_mprotect(start, rewrite_len, final_prot);

	/* The validator's temporary writable mapping must remain non-executable. */
	read_implies_exec = current->personality & READ_IMPLIES_EXEC;
	current->personality &= ~READ_IMPLIES_EXEC;
	/* Check the owning VMAs inside the same transaction as rewriting. */
	if ((final_prot & PROT_EXEC) &&
	    flux_user_range_has_flags(start, rewrite_len, VM_SHARED, false)) {
		ret = -EPERM;
		goto out_restore_personality;
	}
	flux_rewrite_range_publish(start, start + rewrite_len);
	range_published = true;
	ret = sys_mprotect(start, rewrite_len, rewrite_prot);
	if (ret)
		goto out_restore_personality;
	ret = flux_populate_rewrite_range(start, rewrite_len);
	if (ret)
		goto out_failure_prot;
	/*
	 * Linux population can find an existing PTE after mprotect revoked its
	 * host projection. Recreate it before the rewrite backend/rekey reads it.
	 */
	{
		unsigned long addr;

		for (addr = start; addr < start + rewrite_len; addr += PAGE_SIZE)
			if (!flux_repair_present_user_alias_blocking(current->mm, addr)) {
				ret = -EFAULT;
				goto out_failure_prot;
			}
	}
#ifdef CONFIG_FLUX_MPK
	ret = flux_host_dev_rekey_aliases(start, rewrite_len >> PAGE_SHIFT,
					  FLUX_MPK_KERNEL_PKEY);
	kernel_pkey = !ret;
	if (ret)
		goto out_failure_prot;
#endif
	if (final_prot & PROT_EXEC)
		ret = flux_rewrite_exec_serialized((void *)start, rewrite_len);
	else
		ret = flux_invalidate_exec_range(start, rewrite_len, true);
	if (ret)
		goto out_failure_prot;
#ifdef CONFIG_FLUX_MPK
	ret = flux_host_dev_rekey_aliases(start, rewrite_len >> PAGE_SHIFT,
					  FLUX_MPK_APP_PKEY);
	if (ret)
		goto out_failure_prot;
	kernel_pkey = false;
#endif
	ret = sys_mprotect(start, rewrite_len, final_prot);
	if (!ret)
		goto out_restore_personality;

out_failure_prot:
#ifdef CONFIG_FLUX_MPK
	if (kernel_pkey) {
		long rekey_ret;

		rekey_ret = flux_host_dev_rekey_aliases(
			start, rewrite_len >> PAGE_SHIFT,
			FLUX_MPK_APP_PKEY);
		if (!ret)
			ret = rekey_ret;
	}
#endif
	/* Do not return a failed rewrite range to userspace as writable code. */
	sys_mprotect(start, rewrite_len, failure_prot);
out_restore_personality:
	if (range_published)
		flux_rewrite_range_publish(0, 0);
	current->personality |= read_implies_exec;
	return ret;
}

SYSCALL_DEFINE5(flux_perf_event_open,
		struct perf_event_attr __user *, attr_uptr,
		pid_t, pid, int, cpu, int, group_fd, unsigned long, flags)
{
	return -ENODEV;
}

SYSCALL_DEFINE2(flux_process_mrelease, int, pidfd, unsigned int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return sys_process_mrelease(pidfd, flags);
#endif
}

asmlinkage long sys_flux_mmap(unsigned long addr, unsigned long len,
			     unsigned long prot, unsigned long flags,
			     unsigned long fd, unsigned long off);
asmlinkage long sys_flux_mprotect(unsigned long start, size_t len,
				 unsigned long prot);
asmlinkage long sys_flux_mremap(unsigned long addr, unsigned long old_len,
			       unsigned long new_len, unsigned long flags,
			       unsigned long new_addr);
#define __SYSCALL(nr, sym) [nr] = sym,
asmlinkage const void *syscall_table[] __read_mostly = {
#include <asm/syscalls_64.h>
};
static_assert(ARRAY_SIZE(syscall_table) == NR_syscalls);
#undef __SYSCALL

#define __SYSCALL(nr, sym) [nr] = #sym,
asmlinkage const char *syscall_names[] __read_mostly = {
#include <asm/syscalls_64.h>
};
#undef __SYSCALL

#ifdef CONFIG_X86_DEBUG_SYSCALL
#define dbg_sys(nr, a1, a2, a3, a4, a5, a6, ret)                               \
	pr_info("[%3ld][%14s](%llx,%llx,%llx,%llx,%llx,%llx)=%ld\n", (long)nr, \
		syscall_names[nr] + 4, (u64)(a1), (u64)(a2), (u64)(a3),        \
		(u64)(a4), (u64)(a5), (u64)(a6), (long)(ret));
#else
#define dbg_sys(nr, a1, a2, a3, a4, a5, a6, ret) \
	do {                                     \
	} while (0)
#endif

static __always_inline bool do_syscall_inst(struct pt_regs *regs, int nr)
{
	/*
	 * Convert negative numbers to very high and thus out of range
	 * numbers for comparisons.
	 */
	unsigned int unr = nr;
	syscall_ptr_t sys_fn;

	if (likely(unr < NR_syscalls)) {
		unr = array_index_nospec(unr, NR_syscalls);
		sys_fn = (syscall_ptr_t)syscall_table[unr];
		regs->ax = sys_fn(regs->di, regs->si, regs->dx,
				  regs->r10, regs->r8, regs->r9);
		return true;
	}
	return false;
}

static __always_inline bool do_syscall_func(struct pt_regs *regs, int nr)
{
	/*
	 * Convert negative numbers to very high and thus out of range
	 * numbers for comparisons.
	 */
	unsigned int unr = nr;
	syscall_ptr_t sys_fn;

	if (likely(unr < NR_syscalls)) {
		unr = array_index_nospec(unr, NR_syscalls);
		sys_fn = (syscall_ptr_t)syscall_table[unr];
		regs->ax = sys_fn(regs->si, regs->dx, regs->cx,
				  regs->r8, regs->r9, *(u64 *)regs->sp);
		return true;
	}
	return false;
}

static inline void syscall_prepare_regs(struct pt_regs *regs, long nr,
					bool rewritten)
{
	unsigned long return_ip;

	if (rewritten)
		current->thread.fsbase = rdfsbase();
	return_ip = *(u64 *)regs->sp;
	/* save regs pointer */
	current->thread.regs = regs;
	/* syscall must come from user mode; UIF remains live across the CALL. */
	regs->umode = 1;
	/* save original rax */
	regs->orig_ax = regs->ax = nr;
	/* return address has been pushed on the stack */
	if (rewritten)
		regs->uirrv = !!(return_ip & FLUX_REWRITTEN_SYSCALL_TAG);
	regs->ip = rewritten ? return_ip & ~FLUX_REWRITTEN_SYSCALL_TAG : return_ip;
	regs->sp += sizeof(u64);

	/* Raw Linux syscalls preserve SIMD on both fast and slow entry. */
	if (rewritten && regs->uirrv) {
		save_xstate_full(task_xstate(current));
		set_thread_flag(TIF_NEED_FPU_LOAD);
	}
}

/**
 * syscall_entry_fast - very fast path for syscalls.
 */
asmlinkage long syscall_entry_fast(long nr, long a1, long a2, long a3, long a4,
				   long a5, long a6)
{
	unsigned long unr = nr;
	syscall_ptr_t sys_fn;
	long ret;
	unsigned long ti_work;
	struct pt_regs *regs = task_sys_regs(current);
	u64 exec_id = current->self_exec_id;

	syscall_prepare_regs(regs, nr, true);
	arch_enter_from_user_mode(NULL);
	instrumentation_begin();

#ifdef CONFIG_SECCOMP
	/*
	 * Flux's direct-call ABI can reach this entry with the derived syscall
	 * work bit missing after task creation. The seccomp mode is authoritative;
	 * restore its work bit before the generic entry code evaluates filters.
	 */
	if (unlikely(current->seccomp.mode != SECCOMP_MODE_DISABLED))
		set_syscall_work(SECCOMP);
#endif
	if (unlikely(current_thread_info()->syscall_work & SYSCALL_WORK_ENTER))
		nr = syscall_enter_from_user_mode_work(regs, nr);

	unr = nr;
#ifdef CONFIG_FLUX_MPK
	flux_mpk_uaccess_enter();
#endif
	if (likely(unr < NR_syscalls)) {
		unr = array_index_nospec(unr, NR_syscalls);
		sys_fn = (syscall_ptr_t)syscall_table[unr];
		ret = sys_fn(a1, a2, a3, a4, a5, a6);
	} else if (nr == -1) {
		ret = regs->ax;
	} else {
		ret = sys_ni_syscall();
	}

	dbg_sys(unr, a1, a2, a3, a4, a5, a6, ret);

	instrumentation_end();
	/*
	 * Signal mask changes and sends can require delivery before returning.
	 * Use Linux's exit path for these calls, not a second fast signal handler.
	 * User UINTR handles asynchronous delivery and rescheduling as well.
	 */
	switch (nr) {
	case -1: /* Entry work consumed the syscall, e.g. seccomp SIGSYS. */
	case __NR_rt_sigprocmask:
	case __NR_kill:
	case __NR_tkill:
	case __NR_tgkill:
	case __NR_rt_sigqueueinfo:
	case __NR_rt_tgsigqueueinfo:
	case __NR_pidfd_send_signal:
		goto slow_exit;
	}

	/*
	 * Interrupted calls must finish signal/restart work before returning,
	 * including temporary masks left by pselect/ppoll/epoll_pwait on EINTR.
	 * Locally generated signals (e.g. dnotify), exec and notify-resume
	 * (e.g. rseq) also require Linux to prepare the return context.
	 */
	ti_work = read_thread_flags();
	if (unlikely(ret == -EINTR ||
		     (unsigned long)ret + ERESTART_RESTARTBLOCK <=
		     ERESTART_RESTARTBLOCK - ERESTARTSYS ||
		     current->self_exec_id != exec_id ||
		     (ti_work & (_TIF_SIGPENDING | _TIF_NOTIFY_RESUME))))
		goto slow_exit;

#ifdef CONFIG_FLUX_MPK
	flux_mpk_uaccess_exit();
#endif
	flux_restore_user_fpstate(ti_work);
	/*
	 * Host calls restore their captured TLS and task switches install this
	 * task's saved TLS before resuming the C call. No return context changed
	 * on this path, so a nonzero application FSBASE is already live. A zero
	 * FSBASE is the exception: __switch_to uses host TLS while C is running.
	 */
	if (unlikely(!current->thread.fsbase))
		wrfsbase(0);
	arch_exit_to_user_mode();
	return ret;

slow_exit:
	regs->ax = ret;
	syscall_exit_to_user_mode(regs);
#ifdef CONFIG_FLUX_MPK
	flux_mpk_uaccess_exit();
#endif
	syscall_ret_to_user(regs);
	unreachable();
}

/**
 * syscall_entry_slow - slow path for syscalls with arguments
 * in the same registers as the `syscall` instruction.
 */
asmlinkage void syscall_entry_slow(struct pt_regs *regs, int nr)
{
	syscall_prepare_regs(regs, nr, true);
	enter_from_user_mode(regs);
	nr = syscall_enter_from_user_mode_work(regs, nr);
	instrumentation_begin();

#ifdef CONFIG_FLUX_MPK
	flux_mpk_uaccess_enter();
#endif
	if (!do_syscall_inst(regs, nr) && nr != -1)
		regs->ax = sys_ni_syscall();
	dbg_sys(nr, regs->di, regs->si, regs->dx, regs->r10, regs->r8, regs->r9,
		regs->ax);

	instrumentation_end();
	syscall_exit_to_user_mode(regs);
#ifdef CONFIG_FLUX_MPK
	flux_mpk_uaccess_exit();
#endif
	syscall_ret_to_user(regs);
}

#ifdef CONFIG_FLUX_MPK
/* The deferred XOL entry already completed irqentry_enter() and saved SIMD. */
void __noreturn flux_xol_syscall(struct pt_regs *interrupted)
{
	struct pt_regs *regs = task_sys_regs(current);
	int nr = interrupted->ax;

	memcpy(regs, interrupted, sizeof(*regs));
	current->thread.regs = regs;
	regs->orig_ax = nr;
	regs->uirrv = 1; /* Native two-byte syscall restart semantics. */
	regs->ip += 2;
	regs->cx = regs->ip;
	regs->r11 = regs->eflags;
#ifdef CONFIG_SECCOMP
	if (current->seccomp.mode != SECCOMP_MODE_DISABLED)
		set_syscall_work(SECCOMP);
#endif
	nr = syscall_enter_from_user_mode_work(regs, nr);
	instrumentation_begin();
	flux_mpk_uaccess_enter();
	if (!do_syscall_inst(regs, nr) && nr != -1)
		regs->ax = sys_ni_syscall();
	instrumentation_end();
	syscall_exit_to_user_mode(regs);
	flux_mpk_uaccess_exit();
	syscall_ret_to_user(regs);
	unreachable();
}
#endif

/**
 * syscall_entry_env - slow path for syscalls called from Flux
 * Env.
 */
asmlinkage void syscall_entry_env(struct pt_regs *regs, int nr)
{
	struct pt_regs *old_regs = current->thread.regs;
#ifdef CONFIG_FLUX_MPK
	unsigned int uaccess_depth = flux_mpk_uaccess_suspend();
#endif

	syscall_prepare_regs(regs, nr, false);
	enter_from_user_mode(regs);
	nr = syscall_enter_from_user_mode_work(regs, nr);
	instrumentation_begin();

	this_cpu_inc(tls_pcpu.env_syscall_depth);
	if (!do_syscall_func(regs, nr) && nr != -1)
		regs->ax = sys_ni_syscall();
	this_cpu_dec(tls_pcpu.env_syscall_depth);

	dbg_sys(nr, regs->si, regs->dx, regs->cx, regs->r8, regs->r9,
		*(u64 *)regs->sp, regs->ax);

	instrumentation_end();

	current->thread.regs = old_regs;

#ifdef CONFIG_FLUX_MPK
	flux_mpk_uaccess_resume(uaccess_depth);
#endif

	/*
	 * Env syscalls return to Flux runtime C code, not directly to Flux
	 * userspace.  Running the full syscall_exit_to_user_mode() path here can
	 * deliver Flux signals on the runtime C stack saved in regs->sp, corrupting
	 * the host-side loader frame before we jump into the Flux program.  Bounce
	 * context tracking without processing pending user work; real signal
	 * delivery is deferred until an actual Flux-user return path.
	 */
	exit_to_user_mode();
	enter_from_user_mode(regs);
	syscall_ret_to_env(regs);
}

SYSCALL_DEFINE4(flux_pkey_mprotect, unsigned long, start, size_t, len,
		unsigned long, prot, int, pkey)
{
	/* Protection keys belong to the Flux runtime, not application syscalls. */
	return -ENOSYS;
}

SYSCALL_DEFINE2(flux_pkey_alloc, unsigned long, flags,
		unsigned long, init_val)
{
	/* Protection keys belong to the Flux runtime, not application syscalls. */
	return -ENOSYS;
}

SYSCALL_DEFINE1(flux_pkey_free, int, pkey)
{
	/* Protection keys belong to the Flux runtime, not application syscalls. */
	return -ENOSYS;
}

#ifndef CONFIG_FLUX_MPK
SYSCALL_DEFINE1(host_iopl, unsigned int, level)
{
	unsigned long old;
	long ret;

	if (level > 3)
		return -EINVAL;

	old = current->thread.iopl_emul;
	if (level > old) {
		if (!capable(CAP_SYS_RAWIO) ||
		    security_locked_down(LOCKDOWN_IOPORT))
			return -EPERM;
	}

	ret = host_syscall(__NR_iopl, level);
	if (!ret)
		current->thread.iopl_emul = level;
	return ret;
}

SYSCALL_DEFINE3(host_ioperm, unsigned long, from, unsigned long, num, int,
		turn_on)
{
	if ((from + num <= from) || (from + num > FLUX_IOPORT_COUNT))
		return -EINVAL;

	if (turn_on &&
	    (!capable(CAP_SYS_RAWIO) ||
	     security_locked_down(LOCKDOWN_IOPORT)))
		return -EPERM;

	return host_syscall(__NR_ioperm, from, num, turn_on);
}
#endif

/*
 * Rewriting is the architecture operation. VMA permissions, splitting, file
 * checks and errors continue through Linux mprotect. Limit scans to the file's
 * backed pages; an executable mmap beyond EOF must retain ordinary SIGBUS.
 */
static long flux_protect_mapping(unsigned long start, unsigned long len,
				unsigned long prot)
{
	unsigned long end, cursor;
	bool read_implies_exec = (prot & PROT_READ) &&
		(current->personality & READ_IMPLIES_EXEC);
	long ret = 0;

	if (!(prot & PROT_EXEC) && !read_implies_exec)
		return flux_protect_user_range(start, len, prot);
	if (offset_in_page(start) || len > ULONG_MAX - (PAGE_SIZE - 1))
		return -EINVAL;
	len = PAGE_ALIGN(len);
	if (check_add_overflow(start, len, &end))
		return -ENOMEM;
	for (cursor = start; cursor < end;) {
		struct vm_area_struct *vma;
		unsigned long next, scan_end, final_prot = prot;

		mmap_read_lock(current->mm);
		vma = find_vma(current->mm, cursor);
		if (!vma || cursor < vma->vm_start) {
			mmap_read_unlock(current->mm);
			return -ENOMEM;
		}
		next = min(end, vma->vm_end);
		scan_end = next;
		if (read_implies_exec && (vma->vm_flags & VM_MAYEXEC))
			final_prot |= PROT_EXEC;
		if (flux_xol_required((vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)) |
				      calc_vm_prot_bits(final_prot, 0))) {
			bool restore = (vma->vm_flags & VM_EXEC) &&
				       !flux_xol_required(vma->vm_flags);

			mmap_read_unlock(current->mm);
			/* Restore any old private RX rewrite before granting W+X. */
			if (restore) {
				ret = flux_protect_user_range(cursor, next - cursor,
							     PROT_READ | PROT_WRITE);
				if (ret)
					return ret;
			}
			ret = sys_mprotect(cursor, next - cursor, final_prot);
			if (ret)
				return ret;
			cursor = next;
			continue;
		}
		if ((final_prot & PROT_EXEC) && (vma->vm_flags & VM_SHARED)) {
			mmap_read_unlock(current->mm);
			return -EPERM;
		}
		if ((final_prot & PROT_EXEC) && vma->vm_file) {
			loff_t pos = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) +
				     cursor - vma->vm_start;
			loff_t size = i_size_read(file_inode(vma->vm_file));

			if (size <= pos)
				scan_end = cursor;
			else if (size - pos < next - cursor)
				scan_end = cursor + PAGE_ALIGN(size - pos);
		}
		mmap_read_unlock(current->mm);
		if (scan_end > cursor) {
			ret = flux_protect_user_range(cursor, scan_end - cursor,
						       final_prot);
			if (ret)
				return ret;
		}
		if (scan_end < next) {
			ret = sys_mprotect(scan_end, next - scan_end, final_prot);
			if (ret)
				return ret;
		}
		cursor = next;
	}
	return ret;
}

SYSCALL_DEFINE3(flux_mprotect, unsigned long, start, size_t, len,
		unsigned long, prot)
{
	long ret;

	if (flux_rewrite_reserved_range(start, len))
		return start < PAGE_SIZE ?
			(offset_in_page(start) ? -EINVAL : -ENOMEM) : -EPERM;
	mutex_lock(&current->mm->context.host_rewrite_mutex);
	ret = flux_protect_mapping(start, len, prot);
	mutex_unlock(&current->mm->context.host_rewrite_mutex);
	return ret;
}

SYSCALL_DEFINE2(flux_munmap, unsigned long, addr, size_t, len)
{
	long ret;

	if (flux_rewrite_reserved_range(addr, len))
		return -EPERM;
	mutex_lock(&current->mm->context.host_rewrite_mutex);
	ret = vm_munmap(addr, len);
	if (!ret)
		ret = flux_invalidate_exec_range(addr, len, false);
	mutex_unlock(&current->mm->context.host_rewrite_mutex);
	return ret;
}

SYSCALL_DEFINE5(flux_mremap, unsigned long, addr, unsigned long, old_len,
		unsigned long, new_len, unsigned long, flags,
		unsigned long, new_addr)
{
	if (!PAGE_ALIGNED(addr))
		return -EINVAL;
	if (flux_rewrite_reserved_range(addr, old_len) ||
	    ((flags & MREMAP_FIXED) &&
	     flux_rewrite_reserved_range(new_addr, new_len)))
		return -EPERM;
	/*
	 * Linux can relocate only the VMA containing addr. A range extending
	 * into an executable neighbor is rejected by Linux's source-size check;
	 * that neighbor must not turn EFAULT into an architecture policy error.
	 * Moving the executable source itself still needs rewrite relocation.
	 */
	if ((flags || new_len > old_len) &&
	    flux_user_range_has_direct_exec(addr, 1))
		return -EPERM;
	return sys_mremap(addr, old_len, new_len, flags, new_addr);
}

SYSCALL_DEFINE3(flux_madvise, unsigned long, start, size_t, len, int, behavior)
{
	if (!PAGE_ALIGNED(start))
		return -EINVAL;
	if (flux_rewrite_reserved_range(start, len))
		return -EPERM;
	if ((behavior == MADV_DONTNEED || behavior == MADV_DONTNEED_LOCKED ||
	     behavior == MADV_FREE) &&
	    flux_user_range_has_direct_exec(start, len))
		return -EPERM;
	return sys_madvise(start, len, behavior);
}

long flux_sys_mmap(unsigned long addr, unsigned long len, unsigned long prot,
		   unsigned long flags, unsigned long fd, unsigned long off)
{
	unsigned long map_prot = prot;
	unsigned int read_implies_exec = current->personality & READ_IMPLIES_EXEC;
	long ret;

	if (offset_in_page(off))
		return -EINVAL;
	if (!IS_ENABLED(CONFIG_FLUX_MPK) &&
	    (prot & PROT_EXEC) && (flags & MAP_SHARED))
		return -EPERM;
	if ((flags & FLUX_MAP_FIXED_FLAGS) &&
	    flux_rewrite_reserved_range(addr, len))
		return -EPERM;
	if ((prot & PROT_EXEC) &&
	    !(IS_ENABLED(CONFIG_FLUX_MPK) &&
	      ((flags & MAP_SHARED) || flux_wx_prot(prot))))
		map_prot = (prot | PROT_READ | PROT_WRITE) & ~PROT_EXEC;
	mutex_lock(&current->mm->context.host_rewrite_mutex);
	/* Resolve effective executable permissions against the resulting Linux VMA. */
	current->personality &= ~READ_IMPLIES_EXEC;
	ret = ksys_mmap_pgoff(addr, len, map_prot, flags, fd, off >> PAGE_SHIFT);
	current->personality |= read_implies_exec;
	if (!IS_ERR_VALUE(ret)) {
		long err = flux_invalidate_exec_range(ret, len, false);

		if (!err && ((prot & PROT_EXEC) ||
		    (read_implies_exec && (prot & PROT_READ))))
			err = flux_protect_mapping(ret, len, prot);
		if (err) {
			vm_munmap(ret, len);
			ret = err;
		}
	}
	mutex_unlock(&current->mm->context.host_rewrite_mutex);
	return ret;
}

SYSCALL_DEFINE6(flux_mmap, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, off)
{
	return flux_sys_mmap(addr, len, prot, flags, fd, off);
}
