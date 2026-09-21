#define pr_fmt(fmt) "flux_uintr: " fmt

#include <asm/apic.h>
#include <asm/cpufeature.h>
#include <asm/fpu/xcr.h>
#include <asm/fpu/xstate.h>
#include <asm/irq.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/unistd.h>
#include <asm/thread_info.h>
#include <asm/tlbflush.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/ptrace.h>
#include <asm/traps.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/tracepoint.h>

#include "dev.h"
#include "mpk.h"
#include "uintr.h"
#include "compat/x86.h"

#define OS_ABI_REDZONE 128
#define UINTR_ENTRY_SCRATCH 32
#define UINTR_RETURN_SCRATCH 32
#define UINTR_STACK_ADJUST \
	(OS_ABI_REDZONE + UINTR_RETURN_SCRATCH + UINTR_ENTRY_SCRATCH)

#define REX_PREFIX "0x48, "
#define XSAVES ".byte " REX_PREFIX "0x0f,0xc7,0x2f"
#define XRSTORS ".byte " REX_PREFIX "0x0f,0xc7,0x1f"

static __ro_after_init struct uintr_xstate uintr_null_state;
static __ro_after_init struct tracepoint *sched_switch_tp;
static __ro_after_init struct tracepoint *signal_deliver_tp;
static struct uintr_signal_record *uintr_signal_records;
static inline struct uintr_percpu *get_uintr_pcpu(int cpu)
{
	return &per_cpu_ptr(&flux_percpu, cpu)->uintr;
}

static inline struct uintr_percpu *get_uintr_this_cpu(void)
{
	return &this_cpu_ptr(&flux_percpu)->uintr;
}

static void uintr_xrstors(struct uintr_xstate *xs);
static void uintr_xsaves(struct uintr_xstate *xs);
static bool uintr_return_from_kernel(struct uintr_percpu *p,
				     bool notify_pending);

static void __uintr_load_signal_state(struct uintr_percpu *p)
{
	u8 uinv = p->cur_xstate.uintr.misc.uinv;
	bool uif = p->cur_xstate.uintr.misc.uif;
	u64 upid_addr = p->cur_xstate.uintr.upid_addr;
	u64 uirr = p->cur_xstate.uintr.uirr;

	/*
	 * Keep the sender half of the interrupted state live so synchronous
	 * signal repair may execute SENDUIPI.  Only the receiver half is
	 * quarantined for the complete POSIX-frame interval.
	 *
	 * The caller holds local IRQs off.  Restore cur_xstate before they are
	 * enabled so notification handling can only observe the authoritative
	 * interrupted image together with receiver_loaded=false.
	 */
	p->receiver_loaded = false;
	p->cur_xstate.uintr.misc.uinv = 0;
	p->cur_xstate.uintr.misc.uif = false;
	p->cur_xstate.uintr.upid_addr = 0;
	p->cur_xstate.uintr.uirr = 0;
	uintr_xrstors(&p->cur_xstate);
	p->cur_xstate.uintr.misc.uinv = uinv;
	p->cur_xstate.uintr.misc.uif = uif;
	p->cur_xstate.uintr.upid_addr = upid_addr;
	p->cur_xstate.uintr.uirr = uirr;
}

static void uintr_load_signal_state(struct uintr_percpu *p)
{
	unsigned long flags;

	/*
	 * Keep notification IPIs from observing receiver_loaded out of sync with
	 * the hardware image. Posts remain enabled and are folded into
	 * cur_xstate while its receiver is quarantined.
	 */
	local_irq_save(flags);
	__uintr_load_signal_state(p);
	local_irq_restore(flags);
}

void uintr_reclear_signal_uif(void)
{
	struct uintr_percpu *p;

	/*
	 * Signal setup can reload Linux's supervisor FPU image after signal entry
	 * installed the quarantined receiver. Reassert that hardware image
	 * without modifying the saved interrupted receiver or its sender UITT.
	 */
	preempt_disable();
	p = get_uintr_this_cpu();
	if (p->assigned_task == current && !p->is_admin_ctx &&
	    p->sig_uif_depth && !p->receiver_loaded)
		uintr_load_signal_state(p);
	preempt_enable();
}

static void uintr_signal_state_reset(struct uintr_percpu *p)
{
	memset(p->sig_records, 0,
	       sizeof(*p->sig_records) * FLUX_SIGNAL_STACK_SLOTS);
	p->rt_sigreturn_slot = UINTR_SIGNAL_SLOT_NONE;
	p->sig_building_slot = UINTR_SIGNAL_SLOT_NONE;
	p->sig_uif_depth = 0;
}

static void uintr_clear_signal_record(struct uintr_percpu *p,
				      unsigned int slot)
{
	if (WARN_ON_ONCE(slot >= FLUX_SIGNAL_STACK_SLOTS ||
			 p->sig_records[slot].phase == UINTR_SIGNAL_FREE ||
			 !p->sig_uif_depth))
		return;

	memset(&p->sig_records[slot], 0, sizeof(p->sig_records[slot]));
	p->sig_uif_depth--;
}

static void uintr_push_signal_uif(struct uintr_percpu *p, bool uif)
{
	struct uintr_signal_record *record;
	unsigned int slot;

	if (WARN_ON_ONCE(p->sig_uif_depth >= FLUX_SIGNAL_STACK_SLOTS)) {
		/*
		 * Keep overflow fail-closed.  Normal records are frame-keyed, so an
		 * overflow cannot corrupt another frame's saved UIF even if suspended
		 * handlers later return out of creation order.
		 */
		p->sig_building_slot = UINTR_SIGNAL_SLOT_OVERFLOW;
		return;
	}

	for (slot = 0; slot < FLUX_SIGNAL_STACK_SLOTS; slot++)
		if (p->sig_records[slot].phase == UINTR_SIGNAL_FREE)
			break;
	if (WARN_ON_ONCE(slot == FLUX_SIGNAL_STACK_SLOTS)) {
		p->sig_building_slot = UINTR_SIGNAL_SLOT_OVERFLOW;
		return;
	}

	record = &p->sig_records[slot];
	memset(record, 0, sizeof(*record));
	record->phase = UINTR_SIGNAL_CAPTURED;
	record->uif = !!uif;
	p->sig_uif_depth++;
	p->sig_building_slot = slot;
}

bool uintr_complete_signal_frame(unsigned long frame, bool *captured_uif,
				 int *logical_cpu,
				 unsigned long *host_fsbase)
{
	struct uintr_percpu *p = get_uintr_this_cpu();
	unsigned int slot = p->sig_building_slot;
	bool uif;

	*captured_uif = false;
	*logical_cpu = -1;
	*host_fsbase = 0;

	if (slot == UINTR_SIGNAL_SLOT_NONE)
		return false;
	p->sig_building_slot = UINTR_SIGNAL_SLOT_NONE;
	if (p->assigned_ctx)
		*host_fsbase = p->assigned_ctx->host_fsbase;

	if (slot == UINTR_SIGNAL_SLOT_OVERFLOW)
		return true;

	if (WARN_ON_ONCE(slot >= FLUX_SIGNAL_STACK_SLOTS ||
			 p->sig_records[slot].phase == UINTR_SIGNAL_FREE))
		return true;
	uif = !!p->sig_records[slot].uif;

	if (!frame) {
		uintr_clear_signal_record(p, slot);
		if (p->sig_uif_depth) {
			/* The outer POSIX frame still owns receiver quarantine. */
			uintr_load_signal_state(p);
		} else {
			/* No handler will run, so restore the interrupted state. */
			p->cur_xstate.uintr.misc.uif = uif;
			uintr_return_from_kernel(p, uif);
		}
		return true;
	}

	if (WARN_ON_ONCE(p->sig_records[slot].phase != UINTR_SIGNAL_CAPTURED &&
			 p->sig_records[slot].phase != UINTR_SIGNAL_FRAME_READY))
		return true;
	if (p->sig_records[slot].phase == UINTR_SIGNAL_CAPTURED)
		p->sig_records[slot].phase = UINTR_SIGNAL_ACTIVE;
	p->sig_records[slot].frame = frame;
	if (WARN_ON_ONCE(!p->assigned_ctx ||
			 p->assigned_ctx->logical_cpu < 0 ||
			 p->assigned_ctx->logical_cpu >= nr_cpu_ids ||
			 !p->assigned_ctx->host_fsbase))
		return true;

	*captured_uif = uif;
	*logical_cpu = p->assigned_ctx->logical_cpu;
	return true;
}

static void uintr_xrstors(struct uintr_xstate *xs)
{
	int err;
	u64 misc;

	/* XRSTORS #GPs if the current IA32_UINTR_MISC.UINV is non-zero. */
	rdmsrl(MSR_IA32_UINTR_MISC, misc);
	if (misc & (0xffULL << 32))
		wrmsrl(MSR_IA32_UINTR_MISC, misc & ~(0xffULL << 32));

	FLUX_XSTATE_OP(XRSTORS, xs, XFEATURE_MASK_UINTR,
		  ((u64)XFEATURE_MASK_UINTR) >> 32, err);
	if (unlikely(err)) {
		pr_warn_ratelimited(
			"XRSTORS failed err=%d handler=%llx stack=%llx misc=%08x:%02x:%u upid=%llx uirr=%llx uitt=%llx\n",
			err, xs->uintr.handler, xs->uintr.stack_adjust,
			xs->uintr.misc.uitt_size, xs->uintr.misc.uinv,
			xs->uintr.misc.uif, xs->uintr.upid_addr, xs->uintr.uirr,
			xs->uintr.uitt_addr);
	}
}

static void uintr_xsaves(struct uintr_xstate *xs)
{
	int err;

	FLUX_XSTATE_OP(XSAVES, xs, XFEATURE_MASK_UINTR,
		  ((u64)XFEATURE_MASK_UINTR) >> 32, err);
	WARN_ON_ONCE(err);
}

static void uintr_clear_posted(int cpu)
{
	struct uintr_upid *upid = &flux_shm->pcpu[cpu].upid;

	WRITE_ONCE(upid->puir, 0);
	clear_bit(UINTR_UPID_STATUS_ON, &upid->word_val);
}

static u64 uintr_take_posted(struct uintr_upid *upid)
{
	u64 pending = 0;

	/*
	 * Clear ON only after taking PUIR, then recheck.  A concurrent poster
	 * either lands in this loop or observes ON clear and sends a fresh
	 * notification.
	 */
	do {
		pending |= xchg(&upid->puir, 0);
		clear_bit(UINTR_UPID_STATUS_ON, &upid->word_val);
		smp_mb();
	} while (READ_ONCE(upid->puir));

	return pending;
}

static u64 uintr_fold_posted(struct uintr_percpu *p)
{
	struct uintr_upid *upid =
		&flux_shm->pcpu[smp_processor_id()].upid;
	u64 pending = uintr_take_posted(upid);

	p->cur_xstate.uintr.uirr |= pending;
	return pending;
}

static void uintr_detach_hw_state(struct uintr_percpu *p, int cpu)
{
	set_bit(UINTR_UPID_STATUS_SN, &flux_shm->pcpu[cpu].upid.word_val);
	wrmsrl(MSR_IA32_UINTR_MISC, 0);
	uintr_xrstors(&uintr_null_state);
	uintr_clear_posted(cpu);
	p->receiver_loaded = false;
	uintr_signal_state_reset(p);
}

static inline bool uintr_pending(struct uintr_percpu *p)
{
	int cpu = smp_processor_id();

	/* check if an interrupt has been recognized */
	if (p->cur_xstate.uintr.uirr)
		return true;

	/* check if software has posted an interrupt */
	if (flux_shm->pcpu[cpu].upid.puir)
		return true;

	/* check this for good measure, may not be necessary */
	if (test_bit(UINTR_UPID_STATUS_ON, &flux_shm->pcpu[cpu].upid.word_val))
		return true;

	return false;
}

static void uintr_switch_to_kernel(struct uintr_percpu *p)
{
	/* save UINTR state, forces (U)IPIs to land in kernel IPI handler */
	uintr_xsaves(&p->cur_xstate);

	/* detect a UIPI that arrived while we were in the kernel (before xsave) */
	if (uintr_pending(p)) {
		struct task_struct *tsk = p->assigned_task;

		if (tsk)
			set_tsk_thread_flag(tsk, FLUX_TIF_NOTIFY_SIGNAL);
	}
}

/* returns true if an interrupt is pending */
static bool uintr_return_from_kernel(struct uintr_percpu *p,
				      bool notify_pending)
{
	unsigned long flags;
	bool pending;

	local_irq_save(flags);
	uintr_xrstors(&p->cur_xstate);
	p->receiver_loaded = true;
	pending = notify_pending && uintr_pending(p);
	local_irq_restore(flags);

	/*
	 * Prompt the bound task for every pending UINTR after state restoration.
	 * The saved UIF can be transiently clear when the host scheduler preempts
	 * a hardware UINTR handler.  A timer posted while that state is unloaded
	 * otherwise has no notification edge after UIRET restores the interrupted
	 * state.  Pending detection must not turn that hardware entry state into a
	 * runtime delivery gate.
	 */
	if (pending)
		uintr_signal_self();

	return pending;
}

void uintr_deliver_ipi(struct uintr_percpu *p)
{
	struct task_struct *tsk;
	bool live;

	tsk = smp_load_acquire(&p->assigned_task);
	if (!tsk)
		return;

	/*
	 * A notification taken in CPL0 is an ordinary host interrupt, not a
	 * user-interrupt delivery, so hardware has not transferred PUIR into
	 * UIRR.  Do that transfer while this callback owns the notification
	 * edge.  If the receiver is live, bracket the saved-image update with
	 * XSAVES/XRSTORS; if its task is scheduled out, the scheduler hook will
	 * restore the updated image when the task runs again.
	 */
	live = p->receiver_loaded && current == tsk;
	if (live)
		uintr_xsaves(&p->cur_xstate);

	uintr_fold_posted(p);

	if (live)
		uintr_xrstors(&p->cur_xstate);

	set_tsk_thread_flag(tsk, FLUX_TIF_NOTIFY_SIGNAL);
	wake_up_process(tsk);
}

static void uintr_ipi(void)
{
	uintr_deliver_ipi(get_uintr_this_cpu());
}

static void dummy_handler(void)
{
}

static void uintr_ctx_release(struct kref *ref)
{
	struct uintr_ctx *ctx = container_of(ref, struct uintr_ctx, refcount);
	kfree(ctx);
}

void uintr_cleanup_core(struct uintr_percpu *p, int cpu)
{
	struct uintr_ctx *ctx = p->assigned_ctx;
	int vec;

	p->assigned_task = NULL;
	smp_wmb();

	if (!ctx) {
		uintr_detach_hw_state(p, cpu);
		return;
	}

	/* prevent senders from sending to this core */
	for (vec = 0; vec < MAX_NR_USER_VEC; vec++)
		ctx->uitt[vec][cpu].valid = 0;

	uintr_detach_hw_state(p, cpu);

	/* release ref */
	kref_put(&ctx->refcount, uintr_ctx_release);

	p->assigned_ctx = NULL;
}

void uintr_assign_core(struct uintr_ctx *ctx, u64 stack)
{
	int cpu;
	struct uintr_percpu *p;

	cpu = get_cpu();

	/* setup new context for uintr */
	p = get_uintr_this_cpu();
	if (p->assigned_ctx)
		uintr_cleanup_core(p, cpu);

	/*
	 * A retiring iokd/client can race with thread teardown and post one last
	 * notification after the previous context was cleaned.  Do not let stale
	 * PUIR/ON state survive into the next Flux process on this host CPU.
	 */
	set_bit(UINTR_UPID_STATUS_SN, &flux_shm->pcpu[cpu].upid.word_val);
	uintr_clear_posted(cpu);

	p->cur_xstate.uintr.handler = ctx->handler;
	p->cur_xstate.uintr.stack_adjust = stack;
	/*
	 * IA32_UINTR_MISC.UITTSZ is encoded as the highest valid UITT index,
	 * not as the number of entries.  Programming the entry count can make
	 * XRSTORS #GP when restoring the supervisor UINTR state.
	 */
	p->cur_xstate.uintr.misc.uitt_size = nr_cpu_ids * MAX_NR_USER_VEC - 1;
	p->cur_xstate.uintr.misc.uinv = UIPI_APIC_VECTOR;
	/* flux_percpu_entry() enables delivery once after this initial restore. */
	p->cur_xstate.uintr.misc.uif = 0;
	p->cur_xstate.uintr.upid_addr = (u64)&flux_shm->pcpu[cpu].upid;
	p->cur_xstate.uintr.uirr = 0;
	p->cur_xstate.uintr.uitt_addr = (u64)ctx->uitt[0] | 1UL;

	uintr_xrstors(&p->cur_xstate);

	/* allow notifications */
	clear_bit(UINTR_UPID_STATUS_SN, &flux_shm->pcpu[cpu].upid.word_val);

	/* take a reference */
	kref_get(&ctx->refcount);
	p->assigned_ctx = ctx;
	p->assigned_task = current;
	p->receiver_loaded = true;
	p->is_admin_ctx = ctx->is_admin;
	uintr_signal_state_reset(p);

	put_cpu();
}

void uintr_begin_rt_sigreturn(unsigned long frame)
{
	struct uintr_percpu *p = get_uintr_this_cpu();
	unsigned int slot;

	if (p->assigned_task != current || p->is_admin_ctx)
		return;

	for (slot = 0; slot < FLUX_SIGNAL_STACK_SLOTS; slot++)
		if (p->sig_records[slot].phase == UINTR_SIGNAL_ACTIVE &&
		    p->sig_records[slot].frame == frame &&
		    (!p->sig_records[slot].task ||
		     p->sig_records[slot].task == current))
			break;
	if (slot == FLUX_SIGNAL_STACK_SLOTS)
		return;

	if (WARN_ON_ONCE(p->rt_sigreturn_slot != UINTR_SIGNAL_SLOT_NONE ||
			 p->receiver_loaded))
		return;

	/*
	 * Signal entry already detached the receiver.  Mark the exact frame while
	 * Linux consumes it; ordinary host syscalls remain outside UINTR gating.
	 */
	p->rt_sigreturn_slot = slot;
}

void uintr_complete_rt_sigreturn(unsigned long return_ip)
{
	struct uintr_percpu *p;
	unsigned int slot;
	bool synthetic;
	bool uif;

	p = get_uintr_this_cpu();
	if (p->assigned_task != current || p->is_admin_ctx ||
	    p->rt_sigreturn_slot == UINTR_SIGNAL_SLOT_NONE)
		return;
	slot = p->rt_sigreturn_slot;
	if (WARN_ON_ONCE(slot >= FLUX_SIGNAL_STACK_SLOTS ||
			 p->sig_records[slot].phase != UINTR_SIGNAL_ACTIVE ||
			 (p->sig_records[slot].task &&
			  p->sig_records[slot].task != current)))
		return;
	uif = !!p->sig_records[slot].uif;

	synthetic = p->assigned_ctx &&
		    (return_ip == READ_ONCE(p->assigned_ctx->handler) ||
		     return_ip == READ_ONCE(p->assigned_ctx->synthetic_handler));
	p->rt_sigreturn_slot = UINTR_SIGNAL_SLOT_NONE;
	uintr_clear_signal_record(p, slot);

	if (p->sig_uif_depth) {
		/*
		 * Linux may have restored supervisor state while consuming this
		 * nested frame.  The suspended outer POSIX frame still owns the
		 * signal interval, so keep the hardware receiver quarantined.
		 */
		uintr_load_signal_state(p);
		return;
	}

	/*
	 * Linux has consumed the POSIX frame before this hook. A restored
	 * handler RIP denotes the future synthetic route and must enter CPL3 with
	 * UIF still clear. Every normal return restores the captured UIF.
	 */
	p->cur_xstate.uintr.misc.uif = synthetic ? false : uif;
	uintr_return_from_kernel(p, !synthetic && uif);
}

/*
 * Claim one interrupt only from the exact, active outer host signal frame.
 * Receiver quarantine keeps cur_xstate authoritative until rt_sigreturn.
 * The caller has already prepared a protected synthetic entry, and retries
 * the original instruction afterwards. A coincident real store fault is
 * therefore raised again with this pending bit consumed.
 */
long uintr_take_frame_fault(unsigned long frame)
{
	struct uintr_percpu *p;
	struct uintr_signal_record *record;
	unsigned long flags;
	u64 pending;
	long ret = -ENOENT;
	unsigned int slot;

	preempt_disable();
	local_irq_save(flags);
	p = get_uintr_this_cpu();
	if (p->assigned_task != current || p->is_admin_ctx ||
	    p->receiver_loaded || p->sig_uif_depth != 1)
		goto out;
	for (slot = 0; slot < FLUX_SIGNAL_STACK_SLOTS; slot++) {
		record = &p->sig_records[slot];
		if (record->phase != UINTR_SIGNAL_ACTIVE ||
		    record->frame != frame || !record->uif ||
		    (record->task && record->task != current))
			continue;
		pending = record->frame_fault_uirr & p->cur_xstate.uintr.uirr;
		if (!pending)
			break;
		ret = __fls(pending);
		p->cur_xstate.uintr.uirr &= ~BIT_ULL(ret);
		record->frame_fault_uirr = 0;
		/* Return vector + 1, reserving zero for no claim. */
		ret++;
		break;
	}
out:
	local_irq_restore(flags);
	preempt_enable();
	return ret;
}

static void trace_signal_deliver(void *data, int sig,
				 struct kernel_siginfo *info,
				 struct k_sigaction *ka)
{
	struct uintr_percpu *p;
	bool uif;
	unsigned long flags;

	/* Default and ignored dispositions do not create a userspace frame. */
	if (!ka || ka->sa.sa_handler == SIG_DFL || ka->sa.sa_handler == SIG_IGN)
		return;

	/*
	 * Keep the live receiver image on this CPU from XSAVES through the
	 * matching UIF-clear XRSTORS.  Signal delivery cannot reach CPL3 until
	 * this tracepoint returns, so this is the complete atomic entry window.
	 */
	preempt_disable();
	p = get_uintr_this_cpu();
	if (p->assigned_task != current || p->is_admin_ctx)
		goto out_preempt;

	local_irq_save(flags);
	/*
	 * Capture the architectural receiver state while signal delivery is still
	 * in CPL0. Hardware cannot enter the user handler until after this hook.
	 * The outermost entry then quarantines only its receiver for the complete
	 * POSIX-frame lifetime. Posts remain enabled and are folded into the saved
	 * image; sender UITT state remains live for synchronous repair.
	 */
	if (p->receiver_loaded) {
		uintr_xsaves(&p->cur_xstate);
		uif = !p->sig_uif_depth && !!p->cur_xstate.uintr.misc.uif;
	} else {
		/* A nested signal interrupted the quarantined receiver: UIF=0. */
		uif = false;
	}

	uintr_push_signal_uif(p, uif);
	if (uif && p->sig_building_slot < FLUX_SIGNAL_STACK_SLOTS &&
	    sig == SIGSEGV && info &&
	    (info->si_code == SEGV_MAPERR || info->si_code == SEGV_ACCERR ||
	     info->si_code == SEGV_PKUERR) &&
	    current->thread.trap_nr == X86_TRAP_PF &&
	    (current->thread.error_code & 2)) {
		struct pt_regs *regs = task_pt_regs(current);
		unsigned long addr = (unsigned long)info->si_addr;
		unsigned long top = round_down(regs->sp - UINTR_STACK_ADJUST, 16UL);

		if (regs->sp >= UINTR_STACK_ADJUST + 32 &&
		    addr >= top - 32 && addr < top)
			p->sig_records[p->sig_building_slot].frame_fault_uirr =
				p->cur_xstate.uintr.uirr &
				GENMASK_ULL(MAX_NR_USER_VEC - 1, 0);
	}
	if (p->receiver_loaded)
		__uintr_load_signal_state(p);
	local_irq_restore(flags);

out_preempt:
	preempt_enable();
}

/*
 * Some cores may still run other non-managed user tasks. We need to ensure that
 * UINTR notifications are disabled and that those tasks can't send UIPIs. This
 * function is called by the scheduler whenever switching tasks, allowing us to
 * check if UINTR should be temporarily disabled or re-enabled.
 */
static void trace_sched_switch(void *data, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	struct uintr_percpu *p;
	int cpu;

	p = get_uintr_this_cpu();
	cpu = smp_processor_id();

	/* check if this core is currently using UIPI */
	if (!p->assigned_task)
		return;

	/*
	 * Bound task is exiting: release per-cpu binding immediately so
	 * refs/uitt state do not leak after userspace crashes.
	 */
	if (prev == p->assigned_task &&
	    (READ_ONCE(prev->flags) & PF_EXITING ||
	     READ_ONCE(prev->exit_state))) {
		uintr_cleanup_core(p, cpu);
		return;
	}

	if (prev == p->assigned_task) {
		/* A quarantined receiver still has live sender state to detach. */
		if (p->receiver_loaded)
			uintr_switch_to_kernel(p);
		uintr_xrstors(&uintr_null_state);
		p->receiver_loaded = false;
	} else if (next == p->assigned_task) {
		/* Resume sender-only state until the native signal frame is gone. */
		if (p->sig_uif_depth)
			uintr_load_signal_state(p);
		else
			uintr_return_from_kernel(p, true);
	}
}

static struct uintr_ctx *alloc_uintr_ctx(void)
{
	struct uintr_ctx *ctx;
	struct uintr_uitt_entry *uitt;
	struct uintr_uitt_entry *uitt_base;
	size_t uitt_bytes;
	int cpu, vec;

	uitt_bytes = sizeof(*uitt) * nr_cpu_ids * MAX_NR_USER_VEC;
	ctx = kzalloc(sizeof(*ctx) +
			      __alignof__(*uitt) - 1 + uitt_bytes,
		      GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->refcount);
	ctx->logical_cpu = -1;

	/* UITTADDR requires a 16-byte aligned table base. */
	uitt_base = PTR_ALIGN((void *)ctx + sizeof(*ctx), __alignof__(*uitt));

	for (vec = 0; vec < MAX_NR_USER_VEC; vec++) {
		ctx->uitt[vec] = uitt_base + nr_cpu_ids * vec;
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			uitt = &ctx->uitt[vec][cpu];
			uitt->target_upid_addr = (u64)&flux_shm->pcpu[cpu].upid;
			uitt->user_vec = vec;
			uitt->valid = 1;
		}
	}

	return ctx;
}

struct uintr_release_match {
	struct uintr_ctx *ctx;
};

static void uintr_cleanup_if_match(void *data)
{
	struct uintr_release_match *match = data;
	struct uintr_percpu *p = get_uintr_this_cpu();

	if (p->assigned_ctx != match->ctx)
		return;

	uintr_cleanup_core(p, smp_processor_id());
}

void uintr_file_release(struct file *filp)
{
	struct uintr_ctx *ctx = to_uintr_ctx(filp);
	struct uintr_release_match match;
	int cpu;

	if (!ctx)
		return;

	match.ctx = ctx;
	for_each_online_cpu(cpu) {
		if (cpu == smp_processor_id())
			uintr_cleanup_if_match(&match);
		else
			smp_call_function_single(cpu, uintr_cleanup_if_match,
						 &match, 1);
	}

	filp->private_data = NULL;
	kref_put(&ctx->refcount, uintr_ctx_release);
}

long uintr_setup_percpu(struct file *filp, unsigned long arg)
{
	struct uintr_ctx *ctx;
	struct flux_uintr_setup setup;
	struct sighand_struct *sighand;
	unsigned long signal_handler;
	size_t arena_size;
	unsigned long flags;

	if (filp->private_data)
		return -EINVAL;

	ctx = alloc_uintr_ctx();
	if (!ctx)
		return -ENOMEM;

	if (!arg) {
		ctx->is_admin = true;
	} else {
		if (copy_from_user(&setup, (void __user *)arg, sizeof(setup))) {
			kref_put(&ctx->refcount, uintr_ctx_release);
			return -EFAULT;
		}
		if (!setup.handler || !setup.synthetic_handler ||
		    !setup.host_fsbase || !setup.signal_stack ||
		    !setup.signal_stack_slot_size ||
		    check_mul_overflow((size_t)setup.signal_stack_slot_size,
			       (size_t)FLUX_SIGNAL_STACK_SLOTS, &arena_size) ||
		    !access_ok((void __user *)(unsigned long)setup.signal_stack,
			       arena_size) || setup.logical_cpu < 0 ||
		    setup.logical_cpu >= nr_cpu_ids ||
		    (unsigned int)setup.logical_cpu >
			    (unsigned int)FLUX_SIGNAL_ENTRY_COOKIE_CPU_MASK ||
		    setup.reserved) {
			kref_put(&ctx->refcount, uintr_ctx_release);
			return -EINVAL;
		}

		/*
		 * Signals are still blocked and the receiver is not assigned at this
		 * setup boundary.  Validate the trusted host entry and complete frame
		 * arena here, where taking mmap_lock cannot suspend an active POSIX
		 * frame with UIF clear.  Runtime delivery matches the exact handler and
		 * confines every frame to this registered arena, so it needs no blocking
		 * VMA lookup between signal_deliver and rt_sigreturn.
		 */
		sighand = current->sighand;
		spin_lock_irqsave(&sighand->siglock, flags);
		signal_handler =
			(unsigned long)sighand->action[SIGSEGV - 1].sa.sa_handler;
		spin_unlock_irqrestore(&sighand->siglock, flags);
		if (!flux_mpk_range_has_pkey(setup.synthetic_handler, 1,
					     FLUX_MPK_KERNEL_PKEY, VM_EXEC) ||
		    !signal_handler ||
		    !flux_mpk_range_has_pkey(setup.signal_stack, arena_size,
					     FLUX_MPK_KERNEL_PKEY, VM_WRITE) ||
		    !flux_mpk_range_has_pkey(signal_handler, 1,
					     FLUX_MPK_KERNEL_PKEY, VM_EXEC)) {
			kref_put(&ctx->refcount, uintr_ctx_release);
			return -EINVAL;
		}
		ctx->handler = setup.handler;
		ctx->synthetic_handler = setup.synthetic_handler;
		ctx->host_fsbase = setup.host_fsbase;
		ctx->signal_stack = setup.signal_stack;
		ctx->signal_handler = signal_handler;
		ctx->signal_stack_slot_size = setup.signal_stack_slot_size;
		ctx->logical_cpu = setup.logical_cpu;
	}

	/*
	 * Preserve the SysV red zone and leave a separate 32-byte entry area
	 * immediately above the hardware frame.  The Flux MPK entry uses that
	 * area for WRPKRU operands without pushing below the delivered frame.
	 */
	uintr_assign_core(ctx, UINTR_STACK_ADJUST);

	filp->private_data = ctx;
	return 0;
}

struct uintr_trace_hook {
	const char *name;
	struct tracepoint **tp;
	void *probe;
};

static struct uintr_trace_hook uintr_trace_hooks[] = {
	{
		.name = "sched_switch",
		.tp = &sched_switch_tp,
		.probe = trace_sched_switch,
	},
	{
		.name = "signal_deliver",
		.tp = &signal_deliver_tp,
		.probe = trace_signal_deliver,
	},
};

static void __init trace_hooks_reset(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(uintr_trace_hooks); i++)
		*uintr_trace_hooks[i].tp = NULL;
}

static void __cold uintr_scheduler_unhijack(void)
{
	int i;

	for (i = ARRAY_SIZE(uintr_trace_hooks) - 1; i >= 0; i--) {
		struct uintr_trace_hook *hook = &uintr_trace_hooks[i];

		if (*hook->tp)
			tracepoint_probe_unregister(*hook->tp, hook->probe,
						    NULL);
	}
}

static void uintr_teardown(void *info)
{
	u64 xss;

	/* Clear UINTR state */
	wrmsrl(MSR_IA32_UINTR_PD, 0);
	wrmsrl(MSR_IA32_UINTR_TT, 0);
	wrmsrl(MSR_IA32_UINTR_MISC, 0);
	wrmsrl(MSR_IA32_UINTR_HANDLER, 0);
	wrmsrl(MSR_IA32_UINTR_STACKADJUST, 0);

	/* Remove UINTR from supervisor XSTATE */
	rdmsrl(MSR_IA32_XSS, xss);
	xss &= ~BIT_ULL(XFEATURE_UINTR);
	wrmsrl(MSR_IA32_XSS, xss);

	/* Disable UINTR feature entirely */
	cr4_clear_bits(X86_CR4_UINTR);
}

static void __init init_compact_xstate(struct uintr_xstate *xs)
{
	xs->xregs.header.xfeatures = XFEATURE_MASK_UINTR;
	xs->xregs.header.xcomp_bv = XCOMP_BV_COMPACTED_FORMAT |
				    XFEATURE_MASK_UINTR;
}

static __init void uintr_init_cpu(void *info)
{
	bool *failure = (bool *)info;
	struct uintr_upid *upid;
	struct uintr_percpu *p;
	int apicid, cpu;
	u64 xss;

	cpu = smp_processor_id();
	upid = &flux_shm->pcpu[cpu].upid;
	p = get_uintr_this_cpu();
	p->sig_records = uintr_signal_records +
			 cpu * FLUX_SIGNAL_STACK_SLOTS;

	/* suppress notifications */
	set_bit(UINTR_UPID_STATUS_SN, &upid->word_val);
	uintr_clear_posted(cpu);

	/* set normal interrupt vector to use */
	upid->nc.nv = UIPI_APIC_VECTOR;

	apicid = apic->cpu_present_to_apicid(cpu);
	if (apicid == BAD_APICID) {
		*failure = true;
		return;
	}

	if (!x2apic_enabled())
		apicid = ((u32)apicid << 8) & 0xFF00;

	upid->nc.ndst = apicid;

	/* init UINTR */
	cr4_set_bits(X86_CR4_UINTR);

	init_compact_xstate(&p->cur_xstate);
	uintr_signal_state_reset(p);

	/* enable xsave component */
	rdmsrl(MSR_IA32_XSS, xss);
	xss |= BIT_ULL(XFEATURE_UINTR);
	wrmsrl(MSR_IA32_XSS, xss);
}

static void __init tracepoint_finder(struct tracepoint *tp, void *priv)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(uintr_trace_hooks); i++) {
		if (!strcmp(tp->name, uintr_trace_hooks[i].name)) {
			*uintr_trace_hooks[i].tp = tp;
			return;
		}
	}
}

static int __init uintr_scheduler_hijack(void)
{
	int i, ret;

	trace_hooks_reset();
	for_each_kernel_tracepoint(tracepoint_finder, NULL);

	for (i = 0; i < ARRAY_SIZE(uintr_trace_hooks); i++) {
		if (!*uintr_trace_hooks[i].tp)
			return -ENOENT;
	}

	for (i = 0; i < ARRAY_SIZE(uintr_trace_hooks); i++) {
		struct uintr_trace_hook *hook = &uintr_trace_hooks[i];

		ret = tracepoint_probe_register(*hook->tp, hook->probe, NULL);
		if (ret) {
			pr_err("Failed to register tracepoint %s (%d)\n",
			       hook->name, ret);
			goto out_unwind;
		}
	}

	return 0;

out_unwind:
	while (--i >= 0) {
		struct uintr_trace_hook *hook = &uintr_trace_hooks[i];

		tracepoint_probe_unregister(*hook->tp, hook->probe, NULL);
	}

	return ret;
}

int __init uintr_init(void)
{
	int ret;
	bool failure = false;

	uintr_signal_records = kvcalloc(nr_cpu_ids,
				       sizeof(*uintr_signal_records) *
					       FLUX_SIGNAL_STACK_SLOTS,
				       GFP_KERNEL);
	if (!uintr_signal_records)
		return -ENOMEM;
	ret = uintr_scheduler_hijack();
	if (ret)
		goto out_free_records;

	smp_call_function(uintr_init_cpu, &failure, 1);
	uintr_init_cpu(&failure);
	mb();
	if (READ_ONCE(failure)) {
		uintr_scheduler_unhijack();
		smp_call_function(uintr_teardown, NULL, 1);
		uintr_teardown(NULL);
		ret = -EIO;
		goto out_free_records;
	}

	init_compact_xstate(&uintr_null_state);

	/* setup callback for the UINTR interrupt vector (borrowed from KVM) */
	kvm_set_posted_intr_wakeup_handler(uintr_ipi);

	pr_info("enabled");

	return 0;

out_free_records:
	kvfree(uintr_signal_records);
	uintr_signal_records = NULL;
	return ret;
}

static void uintr_cleanup_assigned(void *info)
{
	struct uintr_percpu *p;
	int cpu;

	cpu = smp_processor_id();
	p = get_uintr_this_cpu();
	if (p->assigned_ctx)
		uintr_cleanup_core(p, cpu);
}

void uintr_exit(void)
{
	uintr_scheduler_unhijack();

	smp_call_function(uintr_cleanup_assigned, NULL, 1);
	uintr_cleanup_assigned(NULL);

	smp_call_function(uintr_teardown, NULL, 1);
	uintr_teardown(NULL);

	kvfree(uintr_signal_records);
	uintr_signal_records = NULL;

	kvm_set_posted_intr_wakeup_handler(dummy_handler);

	pr_info("disabled\n");
}
