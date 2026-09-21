#define pr_fmt(fmt) "signal: " fmt

#include <linux/smp.h>
#include <linux/entry-common.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/signal.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/irqflags.h>
#include <linux/pid.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>
#include <asm/extable.h>
#include <asm/host_dev.h>
#include <asm/host_ops.h>
#include <asm/mmu.h>
#include <asm/fault_stats.h>
#include <asm/xol.h>
#include <asm/ioport.h>
#include <asm/mpk_uaccess.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/signal.h>
#include <asm/syscalls.h>
#include <asm/x86/current.h>
#include <asm/x86/syscall.h>
#include <asm/x86/uintr.h>
#include <uapi/asm-generic/ucontext.h>
#include <uapi/asm/flux_ops.h>
#include <uapi/asm/flux_oci.h>
#include <uapi/asm/mpk.h>


DEFINE_PER_CPU(struct flux_sig_list, flux_sig_lists);
static bool flux_sig_initialized = false;
static struct pid *flux_init_pid;
static struct pid *flux_sig_get_init_pid(void);

/*
 * An application may fault with an exhausted or unmapped stack. Its synthetic
 * interrupt must not depend on that stack accepting another store. The task's
 * UINTR stack is idle while application code runs, and host rt_sigreturn keeps
 * UIF clear until the explicitly registered entry consumes this tail.
 * Exact hardware frame-store faults may also interrupt a runtime transition
 * on the user stack. Preserve its PKRU and RIP so UINTR kernel exit resumes
 * that transition without scheduling through live per-CPU return slots.
 */
static bool flux_sig_prepare_protected_stack(struct sigcontext *sc,
					    unsigned int interrupted_pkru,
					    bool uintr_frame_fault)
{
	u64 *frame;

	if (!current->mm || this_cpu_read(tls_pcpu.in_kernel) ||
	    this_cpu_read(tls_pcpu.host_signal_depth) != 1 ||
	    (!uintr_frame_fault && flux_syscall_on_user_return_path(sc->ip)) ||
	    (sc->sp >= (unsigned long)task_stack_page(current) &&
	     sc->sp < task_stack_top(current)))
		return false;
#ifdef CONFIG_FLUX_MPK
	if (interrupted_pkru != FLUX_MPK_APP_PKRU &&
	    (!uintr_frame_fault || interrupted_pkru != FLUX_MPK_KERNEL_PKRU))
		return false;
#endif
	frame = (u64 *)(uintr_stack_top(current) -
		       FLUX_UINTR_SIGNAL_FRAME_BYTES);
	frame[0] = FLUX_UINTR_VECTOR_SIGNAL | FLUX_UINTR_SYNTHETIC_FLAG;
	frame[1] = sc->ip;
	frame[2] = sc->flags;
	frame[3] = sc->sp;
	frame[4] = sc->si;
	frame[5] = sc->ax;
	frame[6] = sc->cx;
	frame[7] = sc->dx;
	frame[8] = interrupted_pkru;
	sc->sp = (unsigned long)frame;
	sc->ip = (unsigned long)flux_uintr_signal_stack;
	return true;
}

static inline bool flux_sig_prepare_uintr_frame(void *ucontext,
					 unsigned int interrupted_pkru)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long orig_sp, frame_sp;
	unsigned long remaining;
	u64 frame[FLUX_UINTR_FRAME_QWORDS];

	if (!uc)
		return false;

	/*
	 * Build the synthetic UINTR tail frame expected by flux_uintr_handler:
	 *   [0] pt_regs.uirrv
	 *   [1] pt_regs.ip
	 *   [2] pt_regs.flags (rflags)
	 *   [3] pt_regs.sp
	 *
	 * flux_uintr_handler will reserve an extra 8-byte orig_ax slot and
	 * then PUSH_ALL to complete struct pt_regs.
	 */
	sc = &uc->uc_mcontext;
	if (flux_sig_prepare_protected_stack(sc, interrupted_pkru, false))
		return true;
	orig_sp = sc->sp;
	/* Use the same red-zone, scratch and alignment as hardware delivery. */
	frame_sp = round_down(orig_sp - FLUX_UINTR_STACK_ADJUST -
				      FLUX_UINTR_FRAME_QWORDS * sizeof(u64),
			      16UL);
	if (frame_sp > orig_sp)
		return false;
#ifdef CONFIG_FLUX_MPK
	if (interrupted_pkru == FLUX_MPK_APP_PKRU &&
	    !flux_mpk_uaccess_range_ok((void __user *)frame_sp,
		FLUX_UINTR_ENTRY_SCRATCH +
		FLUX_UINTR_FRAME_QWORDS * sizeof(u64)))
		return false;
	if (interrupted_pkru != FLUX_MPK_APP_PKRU &&
	    interrupted_pkru != FLUX_MPK_KERNEL_PKRU)
		return false;
#endif
	frame[0] = FLUX_UINTR_VECTOR_SIGNAL |
		   FLUX_UINTR_SYNTHETIC_FLAG;
	frame[1] = sc->ip;
	frame[2] = sc->flags;
	frame[3] = orig_sp;

	/*
	 * A kernel continuation uses the interrupted task's protected stack.
	 * Its bounded frame is kernel memory, not a user-uaccess destination.
	 */
	if (interrupted_pkru == FLUX_MPK_KERNEL_PKRU &&
	    frame_sp >= (unsigned long)task_stack_page(current) &&
	    orig_sp <= task_stack_top(current)) {
		memcpy((void *)frame_sp, frame, sizeof(frame));
		goto prepared;
	}

	/* A failed synthetic-frame store must return through the host frame. */
	pagefault_disable();
	remaining = copy_to_user((void __user *)frame_sp, frame, sizeof(frame));
	pagefault_enable();
	if (remaining)
		return false;

prepared:
	sc->sp = frame_sp;
	sc->ip = (unsigned long)flux_uintr_handler;
	return true;
}

/*
 * The Linux signal frame belongs to the CPU worker pthread.  Flux MM work may
 * schedule another Flux task while that frame remains live, so task_struct is
 * not a stable owner for either nesting or signal-side alias classification.
 */
static bool flux_host_signal_enter(void)
{
	unsigned int depth = this_cpu_read(tls_pcpu.host_signal_depth);

	if (unlikely(depth == (unsigned int)-1))
		return false;
	this_cpu_write(tls_pcpu.host_signal_depth, depth + 1);
	return true;
}

static void flux_host_signal_exit(void)
{
	unsigned int depth = this_cpu_read(tls_pcpu.host_signal_depth);

	if (WARN_ON_ONCE(!depth))
		return;
	this_cpu_write(tls_pcpu.host_signal_depth, depth - 1);
}

static __init int flux_sig_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct flux_sig_list *list = &per_cpu(flux_sig_lists, cpu);

		init_llist_head(&list->pending);
	}

	flux_sig_initialized = true;

	pr_info("signal lists initialized\n");

	return 0;
}
late_initcall(flux_sig_init);

/*
 * Host signal entry always touches only the atomic pending head. A real or
 * synthetic UINTR admitted after UIF becomes set may nest the consumer, so
 * return a detached batch whose cursor stays private to that entry's C stack.
 * Reversing the producer LIFO preserves arrival order within each batch.
 */
struct llist_node *flux_sig_take_batch(int cpu)
{
	struct flux_sig_list *list;
	struct llist_node *batch;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return NULL;

	list = &per_cpu(flux_sig_lists, cpu);
	batch = llist_del_all(&list->pending);
	return batch ? llist_reverse_order(batch) : NULL;
}

static bool flux_sig_is_sync_fault(int sig, const siginfo_t *si)
{
	if (!si || si->si_signo != sig || si->si_code <= 0)
		return false;

	switch (sig) {
	case SIGSEGV:
	case SIGBUS:
	case SIGILL:
	case SIGFPE:
	case SIGTRAP:
		return true;
	default:
		return false;
	}
}

static bool flux_sig_try_fixup_fault(int sig, const siginfo_t *si,
					  void *ucontext)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	struct pt_regs regs = { 0 };
	unsigned long fault_addr;

	if (!flux_sig_is_sync_fault(sig, si) || !uc)
		return false;

	sc = &uc->uc_mcontext;
	fault_addr = sc->cr2 ?: (unsigned long)si->si_addr;

	/*
	 * Host faults raised by Flux uaccess helpers have to be fixed up
	 * against the exact register image at the faulting instruction. In
	 * particular, rep movsb/stosb uses cx/di/si as live in/out operands and
	 * returns the remaining byte count after the exception-table fixup.
	 */
	regs.r15 = sc->r15;
	regs.r14 = sc->r14;
	regs.r13 = sc->r13;
	regs.r12 = sc->r12;
	regs.bp = sc->bp;
	regs.bx = sc->bx;
	regs.r11 = sc->r11;
	regs.r10 = sc->r10;
	regs.r9 = sc->r9;
	regs.r8 = sc->r8;
	regs.cx = sc->cx;
	regs.dx = sc->dx;
	regs.si = sc->si;
	regs.di = sc->di;
	regs.ax = sc->ax;
	regs.ip = sc->ip;
	regs.flags = sc->flags;
	regs.sp = sc->sp;

	if (!fixup_exception(&regs, sc->trapno, sc->err, fault_addr))
		return false;

	sc->r15 = regs.r15;
	sc->r14 = regs.r14;
	sc->r13 = regs.r13;
	sc->r12 = regs.r12;
	sc->bp = regs.bp;
	sc->bx = regs.bx;
	sc->r11 = regs.r11;
	sc->r10 = regs.r10;
	sc->r9 = regs.r9;
	sc->r8 = regs.r8;
	sc->cx = regs.cx;
	sc->dx = regs.dx;
	sc->si = regs.si;
	sc->di = regs.di;
	sc->ax = regs.ax;
	sc->ip = regs.ip;
	sc->flags = regs.flags;
	sc->sp = regs.sp;
	return true;
}

static bool flux_sig_parse_control_signal(int sig, const siginfo_t *si,
					  struct flux_sig_entry *entry)
{
	unsigned int packed;
	unsigned int op;
	unsigned int signo;

	if (!si || sig != FLUX_SIGNAL_CTRL_DOORBELL || si->si_code != SI_QUEUE)
		return false;

	packed = (unsigned int)si->si_value.sival_int;
	op = flux_signal_ctrl_op(packed);
	signo = flux_signal_ctrl_signo(packed);

	if (op == FLUX_SIGNAL_CTRL_NONE)
		return false;

	entry->ctrl_op = (int)op;
	entry->ctrl_arg = flux_signal_ctrl_arg(packed);

	switch (op) {
	case FLUX_SIGNAL_CTRL_KILL:
		if (!valid_signal((int)signo))
			return false;
		entry->sig_nr = (int)signo;
		entry->sig_code = SI_QUEUE;
		entry->sig_addr = 0;
		entry->sig_info_errno = 0;
		entry->sig_info_pid = 0;
		entry->sig_info_uid = 0;
		entry->sig_has_info = true;
		entry->sig_pid = flux_sig_get_init_pid();
		return true;
#ifdef CONFIG_FLUX_RUNC
	case FLUX_SIGNAL_CTRL_EXEC:
	case FLUX_SIGNAL_CTRL_RESOURCE:
		entry->sig_nr = 0;
		entry->sig_code = SI_QUEUE;
		entry->sig_addr = 0;
		entry->sig_has_info = false;
		entry->sig_pid = NULL;
		return true;
#endif
	default:
		return false;
	}
}

static struct pid *flux_sig_get_init_pid(void)
{
	struct pid *pid;

	pid = flux_init_pid;
	if (pid)
		get_pid(pid);

	return pid;
}

static struct pid *flux_sig_get_current_task_pid(int flux_cpu)
{
	struct task_struct *target;

	target = per_cpu(tls_pcpu.current_task, flux_cpu);
	if (!target)
		return NULL;

	return get_task_pid(target, PIDTYPE_PID);
}

void flux_signal_register_init_task(struct task_struct *task)
{
	struct pid *old;
	struct pid *new;

	if (!task)
		return;

	new = get_task_pid(task, PIDTYPE_PID);
	if (!new)
		return;

	old = flux_init_pid;
	flux_init_pid = new;

	if (old)
		put_pid(old);

	pr_info("registered init task pid %d\n", task_pid_nr(task));
}

void flux_signal_unregister_init_task(struct task_struct *task)
{
	struct pid *old = NULL;
	struct pid *task_pid = NULL;

	if (task)
		task_pid = get_task_pid(task, PIDTYPE_PID);

	if (flux_init_pid &&
	    (!task_pid || flux_init_pid == task_pid)) {
		old = flux_init_pid;
		flux_init_pid = NULL;
	}

	if (task_pid)
		put_pid(task_pid);
	if (old)
		put_pid(old);
}

/*
 * Linux owns application VMAs and page tables. Host SIGSEGV/SIGBUS faults
 * enter handle_mm_fault(), and the resulting pages are projected into the
 * active host mm. CONFIG_MMU selects this architecture mechanism.
 */

/*
 * Try to resolve a application page fault (SIGSEGV/SIGBUS) via handle_mm_fault.
 * Returns true if the fault was resolved and the instruction should be retried.
 */
static bool flux_uintr_frame_fault(const struct sigcontext *sc,
				   unsigned long addr)
{
	unsigned long frame_top;
	unsigned long frame_size = FLUX_UINTR_FRAME_BYTES;

	/*
	 * Hardware first applies UISTACKADJUST, then stores its four-qword frame.
	 * A delivery fault reports the interrupted RSP in ucontext and the failed
	 * frame-store address in CR2.
	 */
	if (sc->sp < FLUX_UINTR_STACK_ADJUST + frame_size)
		return false;
	frame_top = round_down(sc->sp - FLUX_UINTR_STACK_ADJUST, 16UL);
	return addr >= frame_top - frame_size && addr < frame_top;
}

/*
 * A valid read-only user stack can reject hardware's implicit UINTR frame
 * stores. Drain a captured pending vector on the protected task stack, then
 * retry the interrupted instruction. Never report the runtime's store as an
 * application SIGSEGV. A real instruction fault remains observable on retry.
 */
static bool flux_redirect_uintr_frame_fault(int sig, siginfo_t *si,
					  void *ucontext,
					  unsigned int interrupted_pkru,
					  int captured_uif)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long old_ip, old_sp, addr;
	long vector;

	if (!captured_uif || sig != SIGSEGV || !si || !uc ||
	    (si->si_code != SEGV_MAPERR && si->si_code != SEGV_ACCERR &&
	     si->si_code != SEGV_PKUERR))
		return false;
	sc = &uc->uc_mcontext;
	addr = sc->cr2 ?: (unsigned long)si->si_addr;
	if (!(sc->err & 2) || !flux_uintr_frame_fault(sc, addr))
		return false;
	old_ip = sc->ip;
	old_sp = sc->sp;
	if (!flux_sig_prepare_protected_stack(sc, interrupted_pkru, true))
		return false;

	/* x86-64 rt_sigframe places its ucontext after the restorer word. */
	vector = flux_host_dev_call_mm(FLUX_DEV_IO_TAKE_FRAME_UINTR,
				      (unsigned long)uc - sizeof(unsigned long));
	if (vector <= 0) {
		sc->ip = old_ip;
		sc->sp = old_sp;
		return false;
	}
	*(u64 *)sc->sp = (vector - 1) | FLUX_UINTR_SYNTHETIC_FLAG;
	return true;
}

static bool flux_try_handle_user_fault(int *sigp, siginfo_t *si, void *ucontext,
				       bool interrupted_in_atomic, bool deferred)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long addr, err;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned int flags = FAULT_FLAG_DEFAULT;
	vm_fault_t fault;
	int sig = *sigp;

	bool mmap_lock_held = false;
	bool vma_lock_held = false;
	bool alias_complete = true;
	bool was_in_kernel, entered_in_kernel;
	bool on_user_return_path, uintr_frame_fault;
	bool new_anon_fault;
	bool pte_was_present;
	bool uffd_wp_fault;

	if ((sig != SIGSEGV && sig != SIGBUS) || !uc || !si)
		return false;
	/*
	 * Only host page faults belong to the SKAS MM fault bridge.  Other x86
	 * exceptions, such as the #GP raised when PR_SET_TSC disables RDTSC,
	 * also arrive as SIGSEGV but carry SI_KERNEL (or another non-page-fault
	 * code).  Treating their stale CR2 value as a page fault can consume the
	 * signal and retry the instruction after the kernel-side handler has
	 * temporarily re-enabled TSC.  SEGV_PKUERR is a page fault too: an SKAS
	 * alias can retain a stale pkey while its Flux PTE already permits the
	 * access, and that narrow case is repaired below.
	 */
	if (sig == SIGSEGV && si->si_code != SEGV_MAPERR &&
	    si->si_code != SEGV_ACCERR && si->si_code != SEGV_PKUERR)
		return false;

	mm = current->mm;
	if (!mm)
		return false;

	sc = &uc->uc_mcontext;
	addr = sc->cr2 ?: (unsigned long)si->si_addr;
	err = sc->err;
	entered_in_kernel = this_cpu_read(tls_pcpu.in_kernel);
	was_in_kernel = !deferred && entered_in_kernel;
	on_user_return_path = flux_syscall_on_user_return_path(sc->ip);
	uintr_frame_fault = flux_uintr_frame_fault(sc, addr);
	if (!was_in_kernel)
		flags |= FAULT_FLAG_USER;
	/*
	 * Only user-address faults are handled here (from application code AND
	 * from flux-kernel uaccess like copy_to_user into skas memory). Faults
	 * on kernel addresses fall through to the uaccess exception fixup.
	 */
	if (!flux_user_alias_addr_valid(addr))
		return false;

	if (err & 0x2)		/* X86_PF_WRITE */
		flags |= FAULT_FLAG_WRITE;
	if (err & 0x10)		/* X86_PF_INSTR */
		flags |= FAULT_FLAG_INSTRUCTION;

	/*
	 * A host SIGSEGV can recur while an outer handler is resolving a Flux
	 * fault because SIGSEGV/SIGBUS use SA_NODEFER.  Do not recursively enter
	 * handle_mm_fault() when the Flux PTE already exists: this case only means
	 * that the active host mm is missing its alias (typically after fork/COW).
	 * Recursing through the full VM fault path can self-deadlock on folio and
	 * runqueue locks held by the outer handler.
	 */
	pte_was_present = flux_user_pte_present(mm, addr);
	uffd_wp_fault = (err & 0x2) &&
		flux_user_pte_uffd_wp(mm, addr);
	/*
	 * sc->err is synthesized by the host signal path and its X86_PF_PROT bit
	 * is not reliable enough to distinguish an absent host alias from a real
	 * write-protection/COW fault.  si_code is the host VM's authoritative
	 * classification: only SEGV_MAPERR means that the active host mm lacks the
	 * alias.  SEGV_ACCERR must take the normal handle_mm_fault() path so the
	 * Flux PTE can be made writable/COW instead of repeatedly reinstalling the
	 * same read-only alias.
	 */
	/*
	 * Match normal Linux uaccess semantics: a kernel access made while an
	 * atomic/scheduler lock was held in the interrupted context must not enter
	 * the sleeping MM path.  Use the entry snapshot rather than live
	 * in_atomic(): flux_signal_handler() adds its own migration pin before it
	 * calls this resolver, and that pin must not classify every app fault atomic.
	 * The host-alias repair ioctl takes a mutex and the host mmap lock, so calling
	 * it here can recurse through schedule_tail()/rseq with the rq lock held
	 * and corrupt the interrupted stack.  Fall through to the exception-table
	 * fixup; a later sleepable/user fault will install the missing alias.
	 *
	 * A hardware UINTR frame-store fault in interrupted application state is
	 * different: it happens before common UINTR entry and returning through
	 * rt_sigreturn would restore UIF and retry the same stores forever.  Let
	 * only that exact application-frame fault reach the synchronous MM path;
	 * kernel, syscall-return and ordinary atomic faults retain the guard.
	 */
	if (interrupted_in_atomic &&
	    (was_in_kernel || on_user_return_path || !uintr_frame_fault)) {
		flux_fault_note(FLUX_FAULT_ATOMIC_GUARD);
		return false;
	}
	/*
	 * A present Flux PTE that already permits the access means only the host
	 * alias is stale.  This includes SEGV_PKUERR: the Flux PTE is the
	 * authoritative application permission, while the host pkey belongs only
	 * to the SKAS alias.  Repair it directly even for the outermost fault.
	 * If the Flux PTE changes between validation and repair, fall through to
	 * the normal fault path instead of misreporting the host-alias race as a
	 * Flux SIGSEGV.  The slow path revalidates the current PTE under the MM
	 * locks and repairs the matching alias before the host context is resumed.
	 * A write to a read-only Flux PTE still takes the normal COW path below.
	 */
	if (pte_was_present && si &&
	    (si->si_code == SEGV_MAPERR ||
	     si->si_code == SEGV_ACCERR || si->si_code == SEGV_PKUERR)) {
		if (flux_repair_present_user_alias_atomic(mm, addr, err)) {
			return true;
		}
	}

	/*
	 * The POSIX fault handler runs on the host pthread stack, not on the
	 * Flux task stack.  It may sleep in handle_mm_fault(), but the Flux task
	 * must not migrate while that host stack is suspended: the source CPU's
	 * pthread can otherwise reuse and overwrite the suspended frames before
	 * the task resumes on another CPU.
	 */
	migrate_disable();

	/* Enter kernel mode before resolving the Flux MM fault. */
	this_cpu_write(tls_pcpu.in_kernel, true);

	/*
	 * A host POSIX signal frame is live here and kmod has cleared UIF.  If a
	 * Flux-user fault sleeps behind another CPU's mmap or page lock, its
	 * wakeup remains a pending UINTR that cannot be consumed before this frame
	 * returns.  Avoid that circular wait: on contention, return through the
	 * libc restorer and retry the original instruction after rt_sigreturn has
	 * restored the interrupted UIF.
	 *
	 * Grow-down expansion and kernel uaccess retain the existing blocking path
	 * for now.  The protected syscall-return window is kernel-origin too: its
	 * deliberate stack prefault runs after arch_exit_to_user_mode() cleared
	 * in_kernel, but must finish before the per-CPU return slot is published.
	 * The common missing-PTE anonymous path uses Linux's per-VMA fault
	 * lock. arch_complete_mmap() prepares its anon_vma in ordinary syscall context,
	 * so an unrelated mmap/mprotect/munmap writer cannot force that fault to retry.
	 * Every other common user-VMA path retains the nonblocking mmap acquisition
	 * and asks the MM fault core not to wait on page locks.
	 *
	 * A missing hardware-UINTR frame page cannot be deferred this way.
	 * rt_sigreturn restores the interrupted UIF before the common UINTR
	 * handler has run, so hardware immediately retries the same frame stores.
	 * Complete that one fault synchronously; otherwise delivery can loop
	 * forever without consuming the notification which would make progress.
	 */
	if (!deferred && !was_in_kernel && !on_user_return_path &&
	    !uintr_frame_fault) {
		vma = NULL;
		if (!pte_was_present) {
			vma = lock_vma_under_rcu(mm, addr);
			if (vma && vma_is_anonymous(vma)) {
				flags |= FAULT_FLAG_VMA_LOCK;
				vma_lock_held = true;
			} else if (vma) {
				vma_end_read(vma);
				vma = NULL;
			}
		}
		if (!vma) {
			flags |= FAULT_FLAG_RETRY_NOWAIT;
			if (!mmap_read_trylock(mm)) {
				fault = VM_FAULT_RETRY;
				goto out;
			}
			mmap_lock_held = true;
			vma = find_vma(mm, addr);
			if (vma && vma->vm_start > addr &&
			    (vma->vm_flags & VM_GROWSDOWN)) {
				mmap_read_unlock(mm);
				mmap_lock_held = false;
				flags &= ~FAULT_FLAG_RETRY_NOWAIT;
				vma = lock_mm_and_find_vma(mm, addr, NULL);
				mmap_lock_held = !!vma;
			} else if (!vma || vma->vm_start > addr) {
				mmap_read_unlock(mm);
				mmap_lock_held = false;
				vma = NULL;
			}
		}
	} else {
		vma = lock_mm_and_find_vma(mm, addr, NULL);
		mmap_lock_held = !!vma;
	}
	if (!vma) {
		flux_fault_note(FLUX_FAULT_VMA_MISSING);
		*sigp = si->si_signo = SIGSEGV;
		si->si_code = SEGV_MAPERR;
		si->si_addr = (void *)addr;
		fault = VM_FAULT_SIGSEGV;
		goto out;
	}
	/*
	 * Enforce VMA access permissions before resolving: a PROT_NONE region
	 * (no R/W/X) or a write to a non-writable VMA must deliver SIGSEGV, not
	 * be silently resolved by handle_mm_fault (which would treat PROT_NONE
	 * as a NUMA hint and let the access through). Instruction faults additionally require VM_EXEC. (A write to a writable VMA with a read-only pte is a
	 * legitimate COW fault and is allowed through.)
	 */
	if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) ||
	    ((err & 0x2) && !(vma->vm_flags & VM_WRITE)) ||
	    ((err & 0x10) && !(vma->vm_flags & VM_EXEC))) {
		flux_fault_note(FLUX_FAULT_VMA_DENIED);
		/* Report the Linux VMA decision, not an internal host alias pkey. */
		*sigp = si->si_signo = SIGSEGV;
		si->si_code = SEGV_ACCERR;
		si->si_addr = (void *)addr;
		if (vma_lock_held) {
			vma_end_read(vma);
			vma_lock_held = false;
		} else if (mmap_lock_held) {
			mmap_read_unlock(mm);
			mmap_lock_held = false;
		}
		fault = VM_FAULT_SIGSEGV;
		goto out;
	}

	/*
	 * handle_userfault() returns on RETRY_NOWAIT before publishing its
	 * waitqueue/message.  An exact UFFD-WP write fault must use Linux's
	 * blocking path so the manager can observe and resolve the event.
	 * All other user faults retain the signal-side NOWAIT policy above.
	 */
	if (uffd_wp_fault)
		flags &= ~FAULT_FLAG_RETRY_NOWAIT;
	new_anon_fault = !pte_was_present && vma_is_anonymous(vma);
	/*
	 * set_ptes() cannot return its host-alias result to handle_mm_fault().  A
	 * signal-side NOWAIT install can therefore lose the alias mutex while the
	 * Flux fault still returns success.  For the anonymous first-touch path,
	 * finish that transaction synchronously with the non-destructive present-PTE
	 * repair before reporting the host fault complete.
	 *
	 * Present-PTE COW and hardware-UINTR frame faults retain their existing
	 * explicit repairs.  The kmod fast path leaves an already matching alias in
	 * place, so this completion does not create a transient PROT_NONE hole.
	 */
	if (!pte_was_present)
		flux_fault_note(flags & FAULT_FLAG_INSTRUCTION ? FLUX_FAULT_MM_MISSING_EXEC :
				flags & FAULT_FLAG_WRITE ? FLUX_FAULT_MM_MISSING_WRITE :
				FLUX_FAULT_MM_MISSING_READ);
	else
		flux_fault_note(flags & FAULT_FLAG_WRITE ? FLUX_FAULT_MM_PRESENT_WRITE :
				FLUX_FAULT_MM_PRESENT_OTHER);
	fault = handle_mm_fault(vma, addr, flags, NULL);
	if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED |
		       VM_FAULT_ERROR | VM_FAULT_SIGSEGV | VM_FAULT_SIGBUS)) &&
	    (pte_was_present || uintr_frame_fault || new_anon_fault)) {
		if (uintr_frame_fault)
			flux_repair_present_user_alias_blocking(mm, addr);
		else if (new_anon_fault)
			alias_complete =
				flux_repair_present_user_alias_blocking(mm, addr);
		else
			/* Present-PTE repair must remain non-destructive. */
			flux_ensure_alias(mm, addr);
	}
	if (vma_lock_held) {
		if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
			vma_end_read(vma);
		vma_lock_held = false;
	}
	/*
	 * VM_FAULT_RETRY normally drops mmap_lock.  RETRY_NOWAIT deliberately
	 * keeps it held so this signal-side caller can unwind without sleeping.
	 */
	if (fault & VM_FAULT_COMPLETED)
		mmap_lock_held = false;
	else if ((fault & VM_FAULT_RETRY) &&
		 !(flags & FAULT_FLAG_RETRY_NOWAIT))
		mmap_lock_held = false;
	if (mmap_lock_held) {
		mmap_read_unlock(mm);
		mmap_lock_held = false;
	}
	if (!alias_complete)
		fault = VM_FAULT_RETRY;

	/*
	 * Match the native page-fault exit path: a userspace allocation that
	 * reached a memcg OOM leaves the OOM synchronization pending until the
	 * architecture calls pagefault_out_of_memory().  Without this step the
	 * task retries the same host SIGSEGV forever instead of selecting and
	 * killing a memcg victim.  Kernel-mode uaccess faults still continue
	 * to the exception-table fixup below.
	 */
	if ((fault & VM_FAULT_OOM) && !was_in_kernel) {
		pagefault_out_of_memory();
		fault &= ~VM_FAULT_OOM;
		/*
		 * Native fault return consumes a fatal signal at its user-exit
		 * boundary.  This handler instead returns through the host
		 * rt_sigreturn, so post a real self UINTR while UIF is still clear.
		 *
		 * Hardware admits it after rt_sigreturn only if interrupted UIF was
		 * set; otherwise it remains pending until a genuine later STUI.
		 */
		if (fatal_signal_pending(current))
			flux_uintr_defer_signal_delivery(
				raw_smp_processor_id());
	}
	out:
	if (fault & VM_FAULT_RETRY)
		flux_fault_note(FLUX_FAULT_RESOLVER_RETRY);
	if (fault & (VM_FAULT_ERROR | VM_FAULT_SIGSEGV))
		flux_fault_note(FLUX_FAULT_RESOLVER_ERROR);
	this_cpu_write(tls_pcpu.in_kernel, entered_in_kernel);
	migrate_enable();

	if (fault & (VM_FAULT_SIGBUS | VM_FAULT_HWPOISON |
		     VM_FAULT_HWPOISON_LARGE)) {
		/*
		 * The host sees a missing skas alias as SIGSEGV even when the
		 * Flux fault resolver determines that the VMA fault is SIGBUS or
		 * HWPOISON (for example, a hugetlb file access beyond i_size or a
		 * MADV_HWPOISON page).  Preserve the Flux MM result and its fault
		 * address when queuing the signal.  Native x86 reports poisoned
		 * pages as SIGBUS/BUS_MCEERR_AR; ordinary SIGBUS faults remain
		 * SIGBUS/BUS_ADRERR.
		 */
		*sigp = SIGBUS;
		if (si) {
			si->si_signo = SIGBUS;
			si->si_code = fault & (VM_FAULT_HWPOISON |
					       VM_FAULT_HWPOISON_LARGE) ?
				      BUS_MCEERR_AR : BUS_ADRERR;
			si->si_addr = (void *)addr;
		}
		return false;
	}
	if (fault & (VM_FAULT_ERROR | VM_FAULT_SIGSEGV))
		return false;
	return true;
}

/*
 * Entry still has UIF clear. Move this fault out of the task's pending slot
 * before an IRQ handler or UIRET can admit a nested entry. Each continuation
 * owns its snapshot, so a nested interrupt cannot consume the outer fault
 * using the nested interrupt's unrelated register frame.
 */
void flux_take_deferred_fault(struct flux_deferred_fault *fault)
{
	struct thread_struct *thread = &current->thread;

	fault->address = thread->fault_address;
	fault->error = thread->fault_error;
	fault->code = thread->fault_code;
	fault->signal = thread->fault_signal;
	thread->fault_signal = 0;
}

#ifdef CONFIG_FLUX_MPK
static unsigned long flux_mpk_cmp64_flags(unsigned long old, u64 lhs, u64 rhs)
{
	u64 result = lhs - rhs;
	unsigned long flags;

	flags = lhs < rhs;
	flags |= !__builtin_parity((unsigned char)result) << 2;
	flags |= (lhs ^ rhs ^ result) & 0x10;
	flags |= (result == 0) << 6;
	flags |= (result >> 63) << 7;
	flags |= (((lhs ^ rhs) & (lhs ^ result)) >> 63) << 11;
	return (old & ~0x8d5UL) | flags;
}

/*
 * Preserve the access width and hardware fault address, including an unaligned
 * load crossing into a missing page. Hold mmap_lock only for VMA validation and
 * this nofault access; the resolver below may wait for a userfault manager.
 */
static int flux_mpk_cmp64_load(unsigned long addr, u64 *value,
			     siginfo_t *si, unsigned long *error, bool trylock)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long end, pos, bad_addr = 0, err = 0;
	u64 data;
	int ret = -EACCES;

	si->si_signo = SIGSEGV;
	si->si_code = SEGV_MAPERR;
	si->si_addr = (void *)addr;
	*error = 0;
	if (!mm || !flux_user_alias_addr_valid(addr) ||
	    check_add_overflow(addr, sizeof(data), &end))
		return ret;
	if (trylock) {
		if (!mmap_read_trylock(mm))
			return -EAGAIN;
	} else {
		mmap_read_lock(mm);
	}
	for (pos = addr; pos < end; pos = min(end, vma->vm_end)) {
		vma = find_vma(mm, pos);
		si->si_addr = (void *)pos;
		if (!vma || vma->vm_start > pos)
			goto out;
		/* Match the ordinary Flux x86 read-fault permission check. */
		if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))) {
			si->si_code = SEGV_ACCERR;
			goto out;
		}
	}
	/* Deferred application work does not carry syscall uaccess depth. */
	if (!flux_mpk_uaccess_range_ok((void __user *)addr, sizeof(data))) {
		for (pos = addr; pos < end; pos++)
			if (!flux_mpk_uaccess_range_ok((void __user *)pos, 1))
				break;
		si->si_addr = (void *)pos;
		si->si_code = SEGV_ACCERR;
		goto out;
	}
	pagefault_disable();
	asm volatile("1: movq (%[addr]), %[data]\n\t"
		     "2:\n\t"
		     _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_FLUX_MPK_READ)
		     : [data] "=&a" (data), "+d" (bad_addr), "+c" (err)
		     : [addr] "r" (addr)
		     : "memory");
	pagefault_enable();
	ret = 0;
	if (err & (1UL << 63)) {
		*error = err & ~(1UL << 63);
		si->si_addr = (void *)bad_addr;
		si->si_code = err & 1 ? SEGV_ACCERR : SEGV_MAPERR;
		ret = -EFAULT;
	} else {
		*value = data;
	}
out:
	mmap_read_unlock(mm);
	return ret;
}

static void flux_complete_mpk_cmp64(struct pt_regs *regs,
				    const struct flux_deferred_fault *cmp)
{
	struct ucontext uc = { };
	siginfo_t si = { };
	unsigned long error;
	u64 rhs;
	int ret, sig;

	while (!fatal_signal_pending(current)) {
		ret = flux_mpk_cmp64_load(cmp->address, &rhs, &si, &error, false);
		if (!ret) {
			regs->flags = flux_mpk_cmp64_flags(regs->flags,
							   cmp->error, rhs);
			regs->ip += cmp->code;
			return;
		}
		sig = si.si_signo;
		uc.uc_mcontext.cr2 = (unsigned long)si.si_addr;
		uc.uc_mcontext.err = error;
		uc.uc_mcontext.ip = regs->ip;
		uc.uc_mcontext.sp = regs->sp;
		uc.uc_mcontext.trapno = 14;
		if (ret != -EFAULT ||
		    !flux_try_handle_user_fault(&sig, &si, &uc, false, true)) {
			force_sig_fault(sig, si.si_code, si.si_addr);
			return;
		}
		cond_resched();
	}
}

static void flux_handle_mpk_cmp64(void *ucontext,
				  const struct flux_mpk_cmp64 *cmp,
				  unsigned int interrupted_pkru, int captured_uif)
{
	struct sigcontext *sc = &((struct ucontext *)ucontext)->uc_mcontext;
	siginfo_t si = { };
	unsigned long error;
	u64 rhs;

	if (captured_uif && this_cpu_read(tls_pcpu.host_signal_depth) == 1 &&
	    !current->thread.fault_signal &&
	    flux_sig_prepare_uintr_frame(ucontext, interrupted_pkru)) {
		/* The existing per-entry snapshot owns these values after UIRET. */
		current->thread.fault_address = cmp->address;
		current->thread.fault_error = cmp->lhs;
		current->thread.fault_code = cmp->length;
		current->thread.fault_signal = SIGILL;
		return;
	}
	/* With UIF clear, complete a resident access without waiting. A busy
	 * map or missing alias leaves the original instruction available to retry. */
	if (!flux_mpk_cmp64_load(cmp->address, &rhs, &si, &error, true)) {
		sc->flags = flux_mpk_cmp64_flags(sc->flags, cmp->lhs, rhs);
		sc->ip += cmp->length;
	}
}
#endif

/*
 * Consume a user fault only after UIRET has ended the host-signal interval.
 * RETRY_NOWAIT is not a no-sleep contract for every Linux vm_ops->fault
 * implementation (notably shmem). Let Linux wait normally with a live receiver.
 */
void flux_complete_user_fault(struct pt_regs *regs,
			      const struct flux_deferred_fault *fault)
{
	struct ucontext uc = { };
	siginfo_t si = { };
	int sig = fault->signal;

	if (!sig)
		return;
#ifdef CONFIG_FLUX_MPK
	if (sig == SIGILL) {
		flux_complete_mpk_cmp64(regs, fault);
		return;
	}
#endif
	si.si_signo = sig;
	si.si_code = fault->code;
	si.si_addr = (void *)fault->address;
	uc.uc_mcontext.cr2 = fault->address;
	uc.uc_mcontext.err = fault->error;
	uc.uc_mcontext.ip = regs->ip;
	uc.uc_mcontext.sp = regs->sp;
	uc.uc_mcontext.trapno = 14;

	if (!flux_try_handle_user_fault(&sig, &si, &uc, false, true))
		force_sig_fault(sig, si.si_code, si.si_addr);
#ifdef CONFIG_FLUX_MPK
	else
		flux_xol_finish_fault(regs, fault);
#endif
}

/*
 * Kernel uaccess needs the interrupted instruction's exception-table fixup,
 * while a fault in the final user-return stack preparation needs user exit.
 * Both run after rt_sigreturn/UIRET, with no host frame holding the receiver.
 */
void flux_complete_kernel_fault(struct pt_regs *regs,
				const struct flux_deferred_fault *fault)
{
	struct ucontext uc = { };
	siginfo_t si = { };
	unsigned long addr = fault->address;
	unsigned long error = fault->error;
	int sig = fault->signal;

	if (!sig)
		return;
	si.si_signo = sig;
	si.si_code = fault->code;
	si.si_addr = (void *)addr;
	uc.uc_mcontext.cr2 = addr;
	uc.uc_mcontext.err = error;
	uc.uc_mcontext.ip = regs->ip;
	uc.uc_mcontext.sp = regs->sp;
	uc.uc_mcontext.trapno = 14;

	/* Keep kernel fault permissions and exception semantics. */
	if (flux_try_handle_user_fault(&sig, &si, &uc, false, false))
		return;
	if (fixup_exception(regs, 14, error, addr))
		return;
	if (flux_syscall_on_user_return_path(regs->ip)) {
		struct pt_regs *user_regs = task_sys_regs(current);

		current->thread.regs = user_regs;
		force_sig_fault(sig, si.si_code, si.si_addr);
		irqentry_exit_to_user_mode(user_regs);
		syscall_ret_to_user(user_regs);
	}
	pr_err("unhandled deferred kernel fault at %lx, address %lx\n",
	       (unsigned long)regs->ip, addr);
	BUG();
}

/*
 * Unlike ordinary application faults, kernel uaccess and the final return
 * stack preparation can block in the MM core. Do not let them schedule while
 * the host signal frame quarantines this CPU's receiver.
 */
static bool flux_defer_kernel_fault(int sig, siginfo_t *si, void *ucontext,
				   unsigned int interrupted_pkru,
				   int captured_uif)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long addr;

	if (!captured_uif || !uc || !si || !current->mm ||
	    interrupted_pkru != FLUX_MPK_KERNEL_PKRU ||
	    this_cpu_read(tls_pcpu.host_signal_depth) != 1 ||
	    current->thread.fault_signal ||
	    (sig != SIGSEGV && sig != SIGBUS))
		return false;
	if (sig == SIGSEGV && si->si_code != SEGV_MAPERR &&
	    si->si_code != SEGV_ACCERR && si->si_code != SEGV_PKUERR)
		return false;

	sc = &uc->uc_mcontext;
	if (sc->sp < (unsigned long)task_stack_page(current) + PAGE_SIZE ||
	    sc->sp >= task_stack_top(current))
		return false;
	addr = sc->cr2 ?: (unsigned long)si->si_addr;
	if (!flux_user_alias_addr_valid(addr))
		return false;
	if (flux_repair_present_user_alias_atomic(current->mm, addr, sc->err))
		return true;
	if (!flux_sig_prepare_uintr_frame(ucontext, interrupted_pkru))
		return false;

	current->thread.fault_address = addr;
	current->thread.fault_error = sc->err;
	current->thread.fault_code = si->si_code;
	current->thread.fault_signal = sig;
	return true;
}

/*
 * Reuse the signal UINTR entry and its post-UIRET continuation. The host frame
 * is consumed by rt_sigreturn before any sleepable page-fault work begins.
 * Present-PTE projection repair keeps its existing atomic fast path.
 */
static bool flux_defer_user_fault(int sig, siginfo_t *si, void *ucontext,
				 unsigned int interrupted_pkru, int captured_uif)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long addr;
	bool prepared;

	if (!captured_uif || !uc || !si || !current->mm ||
	    this_cpu_read(tls_pcpu.in_kernel) ||
	    this_cpu_read(tls_pcpu.host_signal_depth) != 1 ||
	    current->thread.fault_signal ||
	    (sig != SIGSEGV && sig != SIGBUS))
		return false;
	if (sig == SIGSEGV && si->si_code != SEGV_MAPERR &&
	    si->si_code != SEGV_ACCERR && si->si_code != SEGV_PKUERR)
		return false;
	sc = &uc->uc_mcontext;
	addr = sc->cr2 ?: (unsigned long)si->si_addr;
	if (!flux_user_alias_addr_valid(addr) ||
	    flux_syscall_on_user_return_path(sc->ip) ||
	    flux_uintr_frame_fault(sc, addr))
		return false;

	if (flux_repair_present_user_alias_atomic(current->mm, addr, sc->err))
		return true;

	prepared = flux_sig_prepare_uintr_frame(ucontext, interrupted_pkru);
	if (!prepared)
		return false;
	current->thread.fault_address = addr;
	current->thread.fault_error = sc->err;
	current->thread.fault_code = si->si_code;
	current->thread.fault_signal = sig;
	return true;
}

static bool flux_try_handle_kernel_vmalloc_fault(int sig, const siginfo_t *si,
						 void *ucontext)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long addr;
	bool was_in_kernel;
	int ret;

	if ((sig != SIGSEGV && sig != SIGBUS) || !si || !uc)
		return false;
	/*
	 * vmalloc alias repair only handles host page faults.  Other x86
	 * exceptions can also arrive as SIGSEGV with SI_KERNEL and carry a stale
	 * CR2 value.  In particular, the #GP raised for a disabled RDTSC may leave
	 * CR2 inside VMALLOC_START..VMALLOC_END; consuming it here would silently
	 * retry RDTSC after the signal entry path has re-enabled host TSC access.
	 */
	if (sig == SIGSEGV && si->si_code != SEGV_MAPERR &&
	    si->si_code != SEGV_ACCERR)
		return false;
	sc = &uc->uc_mcontext;
	addr = sc->cr2 ?: (unsigned long)si->si_addr;
	if (addr < VMALLOC_START || addr >= VMALLOC_END)
		return false;

	was_in_kernel = this_cpu_read(tls_pcpu.in_kernel);
	migrate_disable();
	this_cpu_write(tls_pcpu.in_kernel, true);
	ret = flux_ensure_kernel_vmalloc_alias(addr, sc->err & 0x2);
	this_cpu_write(tls_pcpu.in_kernel, was_in_kernel);
	migrate_enable();

	/*
	 * A signal frame cannot wait behind the global alias transaction. If its
	 * NOWAIT request loses that race, return through libc rt_sigreturn so the
	 * original instruction retries. Only a permanent mismatch is unhandled.
	 * UIF remains clear throughout; no UINTR is synthesized here.
	 */
	return !ret || ret == -EAGAIN;
}

static void flux_host_signal_unpin_no_resched(void)
{
	/*
	 * The Linux signal frame is still live until libc invokes rt_sigreturn.
	 * The outermost migrate_enable() ends with preempt_enable(), which may
	 * otherwise schedule here and move the suspended Flux task away from
	 * the CPU worker that owns the signal frame and kmod record.
	 *
	 * Hold one outer preemption reference while ending the migration pin,
	 * then drop it without scheduling.  UIF remains clear across the
	 * immediately following libc restorer, so no other Flux entry can
	 * intervene before the host kernel consumes the frame.
	 */
	preempt_disable();
	migrate_enable();
	preempt_enable_no_resched();
}

#ifdef CONFIG_FLUX_FAULT_STATS
static void flux_count_host_fault(int sig, siginfo_t *si, void *ucontext)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long addr;

	if (!READ_ONCE(flux_fault_stats_enabled) || !si || !uc)
		return;
	if (!((sig == SIGSEGV && (si->si_code == SEGV_MAPERR ||
		 si->si_code == SEGV_ACCERR || si->si_code == SEGV_PKUERR)) ||
	      (sig == SIGBUS && (si->si_code == BUS_ADRERR ||
		 si->si_code == BUS_OBJERR))))
		return;
	sc = &uc->uc_mcontext;
	addr = sc->cr2 ?: (unsigned long)si->si_addr;
	if (flux_user_alias_addr_valid(addr))
		flux_fault_note(sc->err & 0x10 ? FLUX_FAULT_HOST_USER_EXEC :
				sc->err & 0x2 ? FLUX_FAULT_HOST_USER_WRITE :
				FLUX_FAULT_HOST_USER_READ);
	else if (addr >= VMALLOC_START && addr < VMALLOC_END)
		flux_fault_note(FLUX_FAULT_HOST_VMALLOC);
	else
		flux_fault_note(FLUX_FAULT_HOST_OTHER_ADDRESS);
	if (this_cpu_read(tls_pcpu.host_signal_depth) > 1)
		flux_fault_note(FLUX_FAULT_HOST_NESTED);
}
#endif

int flux_signal_handler(int sig, void *info, void *ucontext,
			 unsigned int interrupted_pkru, int flux_cpu,
			 int captured_uif)
{
	bool synthetic_uintr = false;
	bool interrupted_in_atomic;
	bool restore_tsc;
	int handled = 0;
	struct flux_sig_list *list;
	struct flux_sig_entry *entry;
	struct task_struct *target;
	siginfo_t *si = info;

	/* Keep the per-CPU ownership below paired across sleepable fault work. */
	/*
	 * Snapshot the interrupted state before this handler adds its own migration
	 * pin.  A nested host signal sees the outer pin and therefore remains
	 * guarded from sleeping MM work.
	 */
	interrupted_in_atomic = in_atomic() || pagefault_disabled();
	migrate_disable();
	if (unlikely(!flux_host_signal_enter())) {
		flux_host_signal_unpin_no_resched();
		return 0;
	}
	restore_tsc = flux_tsc_enter_kernel_mode();

	if (!valid_signal(sig))
		goto out_restore_tsc;
#ifdef CONFIG_FLUX_FAULT_STATS
	flux_count_host_fault(sig, si, ucontext);
#endif
#ifdef CONFIG_FLUX_MPK
	if (sig == SIGSEGV && current->mm && ucontext && si &&
	    ((struct ucontext *)ucontext)->uc_mcontext.trapno == 13 &&
	    interrupted_pkru == FLUX_MPK_APP_PKRU &&
	    !this_cpu_read(tls_pcpu.in_kernel) &&
	    this_cpu_read(tls_pcpu.host_signal_depth) == 1 &&
	    flux_io_signal(&sig, si, ucontext)) {
		handled = 1;
		goto out_restore_tsc;
	}
	if (unlikely(current->thread.xol) && current->mm && ucontext && si &&
	    interrupted_pkru == FLUX_MPK_APP_PKRU &&
	    !this_cpu_read(tls_pcpu.in_kernel) &&
	    this_cpu_read(tls_pcpu.host_signal_depth) == 1 &&
	    flux_xol_signal(&sig, si, ucontext)) {
		handled = 1;
		goto out_restore_tsc;
	}
#endif


#ifdef CONFIG_FLUX_MPK
	if (sig == SIGILL && current->mm &&
	    interrupted_pkru == FLUX_MPK_APP_PKRU &&
	    !this_cpu_read(tls_pcpu.in_kernel)) {
		struct flux_mpk_cmp64 cmp;

		/*
		 * Record publication can relocate the residual table. Population
		 * and application-byte accesses do not hold this lock: a fault
		 * manager may execute residuals while an MM operation waits.
		 * Retry after rt_sigreturn if publication is in progress.
		 */
		preempt_disable();
		if (!mutex_trylock(&current->mm->context.host_rewrite_state_mutex)) {
			handled = 1;
		} else {
			handled = flux_ops_handle_mpk_fault(sig, ucontext, &cmp);
			mutex_unlock(&current->mm->context.host_rewrite_state_mutex);
		}
		preempt_enable_no_resched();
		if (handled == FLUX_MPK_FAULT_CMP64)
			flux_handle_mpk_cmp64(ucontext, &cmp, interrupted_pkru,
					      captured_uif);
		if (handled)
			goto out_restore_tsc;
	}
#endif

	if (!interrupted_in_atomic &&
	    flux_redirect_uintr_frame_fault(sig, si, ucontext, interrupted_pkru,
					    captured_uif))
		goto out_restore_tsc;

	if (flux_try_handle_kernel_vmalloc_fault(sig, si, ucontext))
		goto out_restore_tsc;

	/*
	 * Try skas user-fault handling first: a fault on a valid user VMA
	 * (from application code or flux-kernel uaccess into skas memory) is
	 * resolved by handle_mm_fault. Only if that fails do we fall back to
	 * the uaccess exception fixup (for genuinely bad user pointers).
	 */
	if (!interrupted_in_atomic &&
	    flux_defer_kernel_fault(sig, si, ucontext, interrupted_pkru,
				    captured_uif))
		goto out_restore_tsc;

	if (!interrupted_in_atomic &&
	    flux_defer_user_fault(sig, si, ucontext, interrupted_pkru,
				  captured_uif))
		goto out_restore_tsc;

	if (flux_try_handle_user_fault(&sig, si, ucontext,
				       interrupted_in_atomic, false))
		goto out_restore_tsc;

	if (flux_sig_try_fixup_fault(sig, si, ucontext))
		goto out_restore_tsc;

	if (!flux_sig_initialized)
		goto out_restore_tsc;

	if (flux_cpu < 0 || flux_cpu >= nr_cpu_ids) {
		pr_warn_ratelimited(
			"recovered signal %d for unknown Flux CPU %d\n",
			sig, flux_cpu);
		goto out_restore_tsc;
	}
	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		goto out_restore_tsc;

	entry->sig_nr = sig;
	entry->ctrl_op = FLUX_SIGNAL_CTRL_NONE;
	entry->ctrl_arg = 0;
	entry->sig_code = 0;
	entry->sig_addr = 0;
	entry->sig_has_info = false;
	entry->sig_info_errno = 0;
	entry->sig_info_pid = 0;
	entry->sig_info_uid = 0;
	entry->sig_pid = NULL;

	if (flux_sig_parse_control_signal(sig, si, entry))
		goto out_queue;

	if (si && si->si_signo == sig) {
		entry->sig_code = si->si_code;
		entry->sig_info_errno = si->si_errno;
		entry->sig_info_pid = si->si_pid;
		entry->sig_info_uid = si->si_uid;
		entry->sig_has_info = true;
		switch (sig) {
		case SIGSEGV:
		case SIGBUS:
		case SIGILL:
		case SIGFPE:
		case SIGTRAP:
			entry->sig_addr = (unsigned long)si->si_addr;
			break;
		default:
			break;
		}
	}

	/*
	 * Control signals parsed above are container/runtime requests and pick
	 * their own target, normally init.  Ordinary host-delivered signals are
	 * different: in multiproc mode they may be the host representation of a
	 * Flux task signal, so deliver them to the Flux task that was running
	 * on the interrupted Flux CPU instead of collapsing them onto init.
	 */
	entry->sig_pid = flux_sig_get_current_task_pid(flux_cpu);

	target = per_cpu(tls_pcpu.current_task, flux_cpu);
	if (target && !entry->sig_pid)
		entry->sig_pid = get_task_pid(target, PIDTYPE_PID);

out_queue:
	if (captured_uif)
		synthetic_uintr = flux_sig_prepare_uintr_frame(ucontext,
								 interrupted_pkru);

	list = &per_cpu(flux_sig_lists, flux_cpu);
	llist_add(&entry->sig_node, &list->pending);

	/*
	 * With captured UIF=1, the libc restorer consumes the Linux signal
	 * frame before the host rt_sigreturn resumes at flux_uintr_handler with
	 * UIF still clear. UIRET alone consumes the synthetic hardware frame.
	 *
	 * With captured UIF=0, hardware would not deliver a UINTR. Leave only
	 * pending work for a later hardware-enabled boundary. This also covers
	 * a nested host signal because the outer entry already cleared UIF.
	 * If the interrupted stack cannot accept a synthetic frame, keep the same
	 * work hardware-pending instead of losing the event or recursively faulting
	 * from an unprotected frame store.
	 */
	if (!synthetic_uintr)
		flux_uintr_defer_signal_delivery(flux_cpu);

out_restore_tsc:
	flux_tsc_restore_user_mode(restore_tsc);
	flux_host_signal_exit();
	flux_host_signal_unpin_no_resched();
	return handled;
}
