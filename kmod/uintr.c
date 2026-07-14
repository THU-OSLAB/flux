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
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/tracepoint.h>
#include <linux/version.h>

#include "dev.h"
#include "uintr.h"

#define OS_ABI_REDZONE 128

#define REX_PREFIX "0x48, "
#define XSAVES ".byte " REX_PREFIX "0x0f,0xc7,0x2f"
#define XRSTORS ".byte " REX_PREFIX "0x0f,0xc7,0x1f"

/*
 * PF_IN_SYSCALL tracking moved to uintr_percpu.syscall_nr.
 *
 * Previously this used PF__HOLE__20000000 in task->flags, but host
 * kernel 6.8+ reuses that bit as PF_BLOCK_TS (block I/O timestamp).
 * Writing task->flags from a global tracepoint hook corrupted the
 * block layer's timestamp tracking for ALL host threads (including
 * btrfs transaction workers), causing metadata corruption and
 * filesystem abort.
 */

#ifndef TIF_NOTIFY_SIGNAL
#define TIF_NOTIFY_SIGNAL TIF_SIGPENDING
#endif

#if __has_include(<asm/fpu/internal.h>)
#include <asm/fpu/internal.h>
#endif
#ifndef XSTATE_OP
#define XSTATE_OP(op, st, lmask, hmask, err)                            \
	asm volatile("1:" op "\n\t"                                     \
		     "xor %[err], %[err]\n"                             \
		     "2:\n\t" _ASM_EXTABLE_TYPE(1b, 2b,                 \
						EX_TYPE_FAULT_MCE_SAFE) \
		     : [err] "=a"(err)                                  \
		     : "D"(st), "m"(*st), "a"(lmask), "d"(hmask)        \
		     : "memory")
#endif

__ro_after_init bool uintr_enabled;
static __ro_after_init struct uintr_xstate uintr_null_state;
static __ro_after_init struct tracepoint *sched_switch_tp;
static __ro_after_init struct tracepoint *sys_exit_tp;
static __ro_after_init struct tracepoint *sys_enter_tp;
static __ro_after_init struct tracepoint *signal_deliver_tp;
static __ro_after_init int nouintr;

module_param(nouintr, int, 0);
MODULE_PARM_DESC(nouintr, "disable UINTR");

static inline struct uintr_percpu *get_uintr_pcpu(int cpu)
{
	return &per_cpu_ptr(&flux_percpu, cpu)->uintr;
}

static inline struct uintr_percpu *get_uintr_this_cpu(void)
{
	return &this_cpu_ptr(&flux_percpu)->uintr;
}

/*
 * in_syscall state is tracked via uintr_percpu.syscall_nr:
 *   syscall_nr >= 0  →  in syscall (value is __NR_xxx)
 *   syscall_nr == -1 →  not in syscall
 *
 * These helpers take a uintr_percpu pointer instead of task_struct
 * to avoid touching any host-kernel shared state.
 */
static inline void uintr_mark_in_syscall_pcpu(struct uintr_percpu *p, long id)
{
	p->syscall_nr = id;
}

static inline bool uintr_test_in_syscall_pcpu(struct uintr_percpu *p)
{
	return p->syscall_nr >= 0;
}

static inline void uintr_clear_in_syscall_pcpu(struct uintr_percpu *p)
{
	p->syscall_nr = -1;
}

static inline bool uintr_is_sigreturn_syscall(long id)
{
	switch (id) {
#ifdef __NR_rt_sigreturn
	case __NR_rt_sigreturn:
		return true;
#endif
#ifdef __NR_sigreturn
	case __NR_sigreturn:
		return true;
#endif
#ifdef __X32_SYSCALL_BIT
#ifdef __NR_rt_sigreturn
	case __NR_rt_sigreturn | __X32_SYSCALL_BIT:
		return true;
#endif
#ifdef __NR_sigreturn
	case __NR_sigreturn | __X32_SYSCALL_BIT:
		return true;
#endif
#endif
	default:
		return false;
	}
}

static inline bool uintr_read_uif(void)
{
	u64 misc;

	rdmsrl(MSR_IA32_UINTR_MISC, misc);
	return !!(misc & BIT_ULL(63));
}

static void uintr_xrstors(struct uintr_xstate *xs);
static void uintr_xsaves(struct uintr_xstate *xs);

static inline void uintr_write_uif(struct uintr_percpu *p, bool uif)
{
	u64 misc;

	rdmsrl(MSR_IA32_UINTR_MISC, misc);
	if (uif)
		misc |= BIT_ULL(63);
	else
		misc &= ~BIT_ULL(63);

	wrmsrl(MSR_IA32_UINTR_MISC, misc);
	p->cur_xstate.uintr.misc.uif = !!uif;

	/*
	 * Keep the xstate-backed image aligned with software policy.
	 * The signal path may return to userspace without a syscall-exit
	 * restore point; loading here makes UIF updates observable there.
	 */
	if (p->state_loaded)
		uintr_xrstors(&p->cur_xstate);
}

static inline void uintr_push_signal_uif(struct uintr_percpu *p, bool uif)
{
	if (WARN_ON_ONCE(p->sig_uif_depth >= UINTR_MAX_SIGNAL_NEST))
		return;

	p->sig_uif_stack[p->sig_uif_depth++] = !!uif;
}

static inline bool uintr_pop_signal_uif(struct uintr_percpu *p, bool *uif)
{
	if (WARN_ON_ONCE(!p->sig_uif_depth))
		return false;

	*uif = !!p->sig_uif_stack[--p->sig_uif_depth];
	return true;
}

static void uintr_xrstors(struct uintr_xstate *xs)
{
	int err;

	XSTATE_OP(XRSTORS, xs, XFEATURE_MASK_UINTR,
		  ((u64)XFEATURE_MASK_UINTR) >> 32, err);
	WARN_ON_ONCE(err);
}

static void uintr_xsaves(struct uintr_xstate *xs)
{
	int err;

	XSTATE_OP(XSAVES, xs, XFEATURE_MASK_UINTR,
		  ((u64)XFEATURE_MASK_UINTR) >> 32, err);
	WARN_ON_ONCE(err);
}

static inline bool uintr_pending(struct uintr_percpu *p, bool ignore_uif)
{
	int cpu = smp_processor_id();

	/* ignore interrupts if user interrupt flag is 0 */
	if (!ignore_uif && !p->cur_xstate.uintr.misc.uif)
		return false;

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

	/*
	 * Signal handlers run with UIF=0 by policy. Keep the saved state in sync
	 * so syscall/schedule return paths cannot re-enable UIF.
	 */
	if (p->sig_uif_depth)
		p->cur_xstate.uintr.misc.uif = 0;

	/* detect a UIPI that arrived while we were in the kernel (before xsave) */
	if (uintr_pending(p, false))
		set_tsk_thread_flag(p->assigned_task, TIF_NOTIFY_SIGNAL);
}

/* returns true if an interrupt is pending */
static bool uintr_return_from_kernel(struct uintr_percpu *p)
{
	bool ignore_uif = !p->sig_uif_depth;

	uintr_xrstors(&p->cur_xstate);
	p->state_loaded = true;

	if (uintr_pending(p, ignore_uif)) {
		uintr_signal_self();
		return true;
	}

	return false;
}

void uintr_deliver_ipi(struct uintr_percpu *p)
{
	struct task_struct *tsk;

	tsk = smp_load_acquire(&p->assigned_task);
	if (!tsk)
		return;

	set_tsk_thread_flag(tsk, TIF_NOTIFY_SIGNAL);
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
	int vec;

	p->assigned_task = NULL;
	smp_wmb();

	/* prevent senders from sending to this core */
	for (vec = 0; vec < MAX_NR_USER_VEC; vec++)
		p->assigned_ctx->uitt[vec][cpu].valid = 0;

	/* suppress notifications */
	set_bit(UINTR_UPID_STATUS_SN, &flux_shm->pcpu[cpu].upid.word_val);

	/* load NULL state */
	wrmsrl(MSR_IA32_UINTR_MISC, 0);
	uintr_xrstors(&uintr_null_state);

	/* remove any existing interrupts */
	flux_shm->pcpu[cpu].upid.puir = 0;
	clear_bit(UINTR_UPID_STATUS_ON, &flux_shm->pcpu[cpu].upid.word_val);

	/* release ref */
	kref_put(&p->assigned_ctx->refcount, uintr_ctx_release);

	p->assigned_ctx = NULL;
	p->state_loaded = false;
	p->sig_uif_depth = 0;
	p->syscall_nr = -1;
}

void uintr_assign_core(struct uintr_ctx *ctx, u64 stack)
{
	int cpu;
	struct uintr_percpu *p;

	cpu = get_cpu();

	/* setup new context for uintr */
	p = get_uintr_this_cpu();
	p->cur_xstate.uintr.handler = ctx->handler;
	p->cur_xstate.uintr.stack_adjust = stack;
	p->cur_xstate.uintr.misc.uitt_size = nr_cpu_ids * MAX_NR_USER_VEC;
	p->cur_xstate.uintr.misc.uinv = UIPI_APIC_VECTOR;
	p->cur_xstate.uintr.misc.uif = 0; /* enable it in user space */
	p->cur_xstate.uintr.upid_addr = (u64)&flux_shm->pcpu[cpu].upid;
	p->cur_xstate.uintr.uirr = 0;
	p->cur_xstate.uintr.uitt_addr = (u64)ctx->uitt[0] | 1UL;

	/* syscall return will restore the context */
	if (!uintr_test_in_syscall_pcpu(p)) {
		/* the first time a thread attaches the syscall isn't marked */
		uintr_xrstors(&p->cur_xstate);
	}

	/* allow notifications */
	clear_bit(UINTR_UPID_STATUS_SN, &flux_shm->pcpu[cpu].upid.word_val);

	/* take a reference */
	kref_get(&ctx->refcount);
	p->assigned_ctx = ctx;
	p->assigned_task = current;
	p->state_loaded = true;
	p->is_admin_ctx = ctx->is_admin;
	p->sig_uif_depth = 0;
	p->syscall_nr = -1;

	put_cpu();
}

/*
 * trace_sys_enter - called when any process on the machine enters a system
 * call. Marks the task as in a system call, and ensures that UIPIs are
 * delivered to the kernel IPI handler.
 */
static void trace_sys_enter(void *data, struct pt_regs *regs, long id)
{
	struct uintr_percpu *p;

	p = get_uintr_this_cpu();

	/* not our UINTR task */
	if (p->assigned_task != current)
		return;

	/* the admin context doesn't receive interrupts */
	if (p->is_admin_ctx)
		return;

	uintr_mark_in_syscall_pcpu(p, id);

	WARN_ON_ONCE(!p->state_loaded);

	uintr_switch_to_kernel(p);
}

/* called when any process on the machine returns from a system call */
static void trace_sys_exit(void *data, struct pt_regs *regs, long ret)
{
	struct uintr_percpu *p;
	bool pending;
	bool uif;
	long syscall_nr;

	p = get_uintr_this_cpu();

	if (p->assigned_task != current || !uintr_test_in_syscall_pcpu(p))
		return;

	syscall_nr = p->syscall_nr;
	uintr_clear_in_syscall_pcpu(p);

	/* fully restore uintr context, check if a UIPI is pending */
	pending = uintr_return_from_kernel(p);

	if (uintr_is_sigreturn_syscall(syscall_nr) &&
	    uintr_pop_signal_uif(p, &uif)) {
		uintr_write_uif(p, uif);
		if (uif && uintr_pending(p, false))
			uintr_signal_self();
	}

	if (!pending)
		return;

	/* a UIPI is pending, break the system call out of a potential restart */
	switch (regs->ax) {
	case -ERESTARTNOHAND:
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTART_RESTARTBLOCK:
		regs->ax = -EINTR;
		break;
	}
}

static void trace_signal_deliver(void *data, int sig, struct kernel_siginfo *info,
				 struct k_sigaction *ka)
{
	struct uintr_percpu *p;
	bool uif;

	/* Ignore default/ignored actions, we only care about user handlers. */
	if (!ka || ka->sa.sa_handler == SIG_DFL || ka->sa.sa_handler == SIG_IGN)
		return;

	p = get_uintr_this_cpu();
	if (p->assigned_task != current || p->is_admin_ctx)
		return;

	/*
	 * Kernel-mode MSR view is not necessarily the user-return UIF source.
	 * Refresh from the tracked xstate image first and push that value.
	 */
	if (p->state_loaded)
		uintr_xsaves(&p->cur_xstate);

	uif = !!p->cur_xstate.uintr.misc.uif;
	uintr_push_signal_uif(p, uif);
	uintr_write_uif(p, false);
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

	/* check if the next task is not our target task */
	if (next != p->assigned_task) {
		if (!p->state_loaded)
			return;

		if (!uintr_test_in_syscall_pcpu(p))
			uintr_switch_to_kernel(p);

		/* clear all the UINTR state before entering the next task */
		uintr_xrstors(&uintr_null_state);
		p->state_loaded = false;

	} else if (!p->state_loaded) {
		/* we are switching back to the UINTR task, reload the state */
		if (!uintr_test_in_syscall_pcpu(p))
			uintr_return_from_kernel(p);
	}
}

static struct uintr_ctx *alloc_uintr_ctx(void)
{
	struct uintr_ctx *ctx;
	struct uintr_uitt_entry *uitt;
	int cpu, vec;

	ctx = kzalloc(sizeof(*ctx) +
			      sizeof(*uitt) * nr_cpu_ids * MAX_NR_USER_VEC,
		      GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->refcount);

	for (vec = 0; vec < MAX_NR_USER_VEC; vec++) {
		ctx->uitt[vec] = (void *)ctx + sizeof(*ctx) +
				 sizeof(*uitt) * nr_cpu_ids * vec;
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
	struct task_struct *task;
	struct uintr_ctx *ctx;
};

static void uintr_cleanup_if_match(void *data)
{
	struct uintr_release_match *match = data;
	struct uintr_percpu *p = get_uintr_this_cpu();

	if (p->assigned_task != match->task || p->assigned_ctx != match->ctx)
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

	match.task = current;
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

long uintr_setup_percpu(struct file *filp, unsigned long handler)
{
	struct uintr_ctx *ctx;

	if (!uintr_enabled)
		return -ENODEV;

	if (filp->private_data)
		return -EINVAL;

	ctx = alloc_uintr_ctx();
	if (!ctx)
		return -ENOMEM;

	if (handler)
		ctx->handler = handler;
	else
		ctx->is_admin = true;

	uintr_assign_core(ctx, OS_ABI_REDZONE);

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
		.name = "sys_exit",
		.tp = &sys_exit_tp,
		.probe = trace_sys_exit,
	},
	{
		.name = "sys_enter",
		.tp = &sys_enter_tp,
		.probe = trace_sys_enter,
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
	p->syscall_nr = -1;

	/* suppress notifications */
	set_bit(UINTR_UPID_STATUS_SN, &upid->word_val);

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

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_UINTR)) {
		pr_info("not enabled (no support)");
		return 0;
	}

	if (nouintr) {
		pr_info("not enabled (disabled by module param)");
		return 0;
	}

	ret = uintr_scheduler_hijack();
	if (ret)
		return ret;

	smp_call_function(uintr_init_cpu, &failure, 1);
	uintr_init_cpu(&failure);
	mb();
	if (READ_ONCE(failure)) {
		uintr_scheduler_unhijack();
		smp_call_function(uintr_teardown, NULL, 1);
		uintr_teardown(NULL);
		return -1;
	}

	init_compact_xstate(&uintr_null_state);

	/* setup callback for the UINTR interrupt vector (borrowed from KVM) */
	kvm_set_posted_intr_wakeup_handler(uintr_ipi);

	uintr_enabled = true;

	pr_info("enabled");

	return 0;
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
	if (!uintr_enabled)
		return;

	uintr_scheduler_unhijack();

	smp_call_function(uintr_cleanup_assigned, NULL, 1);
	uintr_cleanup_assigned(NULL);

	smp_call_function(uintr_teardown, NULL, 1);
	uintr_teardown(NULL);

	kvm_set_posted_intr_wakeup_handler(dummy_handler);

	pr_info("disabled\n");
}
