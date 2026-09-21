#define pr_fmt(fmt) "flux_mpk: " fmt

#include <asm/processor.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/types.h>
#include <asm/sigframe.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/unistd.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <asm/mmu_context.h>
#include <asm/pkeys.h>
#include <asm/pkru.h>
#include <asm/sighandling.h>

#include "dev.h"
#include "hook.h"
#include "mm.h"
#include "mpk.h"
#include "uintr.h"

static long (*orig_x64_sys_call)(const struct pt_regs *regs, unsigned int nr);
static long (*orig_x64_sys_rt_sigreturn)(const struct pt_regs *regs);
static int (*orig_x64_setup_rt_frame)(struct ksignal *ksig,
				      struct pt_regs *regs);
static bool (*orig_copy_fpstate_to_sigframe)(void __user *buf,
					     void __user *buf_fx, int size,
					     u32 pkru);
static void (*orig_fpu_clear_user_states)(struct fpu *fpu);
static unsigned int flux_mpk_pkru_xstate_offset;
static DEFINE_MUTEX(flux_mpk_hook_lock);
static atomic_t flux_mpk_active_calls = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(flux_mpk_active_waitq);
static unsigned int flux_mpk_users;
static bool flux_mpk_hooks_installed;
static bool flux_uintr_signal_hooks_installed;
static bool flux_mpk_disable_pending;
static bool flux_mpk_module_ref;

static struct flux_hook_group mpk_hook_group;
static struct flux_hook_group uintr_signal_hook_group;
static int flux_mpk_state_init(void);
static int flux_mpk_disable_thread(void *unused);

static void flux_mpk_schedule_disable_locked(void)
{
	struct task_struct *task;

	if (flux_mpk_disable_pending)
		return;

	flux_mpk_disable_pending = true;
	task = kthread_run(flux_mpk_disable_thread, NULL, "flux_mpk_cleanup");
	if (IS_ERR(task)) {
		flux_mpk_disable_pending = false;
		pr_err("failed to start cleanup thread: %ld\n", PTR_ERR(task));
	}
}

static bool flux_mpk_patch_signal_pkru(void __user *buf_fx, int size, u32 pkru);

static void flux_mpk_call_done(void)
{
	if (atomic_dec_and_test(&flux_mpk_active_calls))
		wake_up_all(&flux_mpk_active_waitq);
}

static struct uintr_percpu *flux_mpk_uintr_this_cpu(void)
{
	return &raw_cpu_ptr(&flux_percpu)->uintr;
}

static struct uintr_signal_record *
flux_mpk_signal_state_find_phase(enum uintr_signal_phase phase)
{
	struct uintr_percpu *p = flux_mpk_uintr_this_cpu();
	unsigned int i;

	for (i = 0; i < FLUX_SIGNAL_STACK_SLOTS; i++) {
		struct uintr_signal_record *state = &p->sig_records[i];

		if (state->phase == phase && state->task == current)
			return state;
	}
	return NULL;
}

static struct uintr_signal_record *
flux_mpk_signal_state_find_sigreturn(const struct pt_regs *regs)
{
	struct uintr_percpu *p = flux_mpk_uintr_this_cpu();
	unsigned int i;

	for (i = 0; i < FLUX_SIGNAL_STACK_SLOTS; i++) {
		struct uintr_signal_record *state = &p->sig_records[i];

		if (state->phase == UINTR_SIGNAL_ACTIVE &&
		    state->task == current &&
		    regs->sp == state->frame + sizeof(unsigned long))
			return state;
	}
	return NULL;
}

static struct uintr_signal_record *flux_mpk_signal_state_begin(int *err)
{
	struct uintr_percpu *p = flux_mpk_uintr_this_cpu();
	unsigned int slot = p->sig_building_slot;
	struct uintr_signal_record *state;

	if (slot == UINTR_SIGNAL_SLOT_OVERFLOW) {
		*err = -EOVERFLOW;
		return NULL;
	}
	if (WARN_ON_ONCE(slot >= FLUX_SIGNAL_STACK_SLOTS)) {
		*err = -EFAULT;
		return NULL;
	}
	state = &p->sig_records[slot];
	if (WARN_ON_ONCE(state->phase != UINTR_SIGNAL_CAPTURED)) {
		*err = -EFAULT;
		return NULL;
	}
	return state;
}

static bool flux_mpk_worker_current(void)
{
	return READ_ONCE(flux_mpk_uintr_this_cpu()->assigned_task) == current;
}

bool flux_mpk_range_has_pkey(unsigned long start, unsigned long len,
				    int pkey, unsigned long required_flags)
{
	struct vm_area_struct *vma;
	unsigned long cursor = start;
	unsigned long end;
	bool valid = false;

	if (!current->mm || !len || check_add_overflow(start, len, &end))
		return false;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, start);
	while (vma && cursor < end) {
		if (vma->vm_start > cursor || vma_pkey(vma) != pkey ||
		    (vma->vm_flags & required_flags) != required_flags)
			goto out;

		cursor = min(end, vma->vm_end);
		if (cursor < end)
			vma = find_vma(current->mm, cursor);
	}
	valid = cursor == end;
out:
	mmap_read_unlock(current->mm);
	return valid;
}

static bool flux_mpk_range_within(unsigned long start, unsigned long len,
				  unsigned long outer_start,
				  unsigned long outer_len)
{
	unsigned long end, outer_end;

	return len && !check_add_overflow(start, len, &end) &&
	       !check_add_overflow(outer_start, outer_len, &outer_end) &&
	       start >= outer_start && end <= outer_end;
}

static bool flux_mpk_enabled_current(void)
{
	return flux_mm_mpk_enabled_current_rcu();
}

static bool flux_mpk_restricted_current(void)
{
	return flux_mpk_enabled_current() &&
	       (read_pkru() & FLUX_MPK_PROTECTED_MASK);
}

static bool flux_mpk_prepare_sigreturn(const struct pt_regs *regs,
				       struct uintr_signal_record *state)
{
	struct xregs_state __user *xsave = state->fpstate;
	u64 xfeatures, xcomp_bv;

	if (state->phase != UINTR_SIGNAL_ACTIVE || state->task != current ||
	    read_pkru() != FLUX_MPK_KERNEL_PKRU ||
	    regs->sp != state->frame + sizeof(unsigned long) ||
	    !flux_mpk_range_within(state->frame, sizeof(struct rt_sigframe),
				   state->stack_start, state->stack_size) ||
	    !flux_mpk_range_within((unsigned long)state->fpstate,
				   state->fpstate_size, state->stack_start,
				   state->stack_size))
		return false;

	if (__get_user(xfeatures, &xsave->header.xfeatures) ||
	    __get_user(xcomp_bv, &xsave->header.xcomp_bv) ||
	    !(xfeatures & XFEATURE_MASK_PKRU) ||
	    (xcomp_bv & XCOMP_BV_COMPACTED_FORMAT))
		return false;

	return flux_mpk_patch_signal_pkru(state->fpstate, state->fpstate_size,
					  state->expected_pkru);
}

static long hook_x64_sys_call(const struct pt_regs *regs, unsigned int nr)
{
	long ret;

	if (unlikely(flux_mpk_restricted_current())) {
		ret = -EPERM;
		goto out;
	}

	ret = orig_x64_sys_call(regs, nr);
out:
	flux_mpk_call_done();
	return ret;
}

static long hook_x64_sys_rt_sigreturn(const struct pt_regs *regs)
{
	struct uintr_signal_record *state;
	long ret;

	state = flux_mpk_signal_state_find_sigreturn(regs);
	if (state) {
		if (!flux_mpk_prepare_sigreturn(regs, state)) {
			ret = -EPERM;
			goto out;
		}
	} else if (flux_mpk_signal_state_find_phase(UINTR_SIGNAL_ACTIVE)) {
		/* Never consume another suspended frame on an unmatched return. */
		ret = -EPERM;
		goto out;
	}

	uintr_begin_rt_sigreturn(regs->sp - sizeof(unsigned long));
	ret = orig_x64_sys_rt_sigreturn(regs);
	uintr_complete_rt_sigreturn(regs->ip);
out:
	flux_mpk_call_done();
	return ret;
}

static bool flux_mpk_patch_signal_pkru(void __user *buf_fx, int size, u32 pkru)
{
	struct xregs_state __user *xsave = buf_fx;
	struct pkru_state pkru_state = {
		.pkru = pkru,
	};
	u64 xfeatures;

	if (size < 0 || flux_mpk_pkru_xstate_offset > (unsigned int)size ||
	    sizeof(pkru_state) >
		    (unsigned int)size - flux_mpk_pkru_xstate_offset)
		return false;

	if (__get_user(xfeatures, &xsave->header.xfeatures))
		return false;
	xfeatures |= XFEATURE_MASK_PKRU;
	if (__put_user(xfeatures, &xsave->header.xfeatures))
		return false;
	if (copy_to_user((char __user *)buf_fx + flux_mpk_pkru_xstate_offset,
			 &pkru_state, sizeof(pkru_state)))
		return false;

	return true;
}

static bool hook_copy_fpstate_to_sigframe(void __user *buf, void __user *buf_fx,
					  int size, u32 pkru)
{
	struct uintr_signal_record *state =
		flux_mpk_signal_state_find_phase(UINTR_SIGNAL_MPK_BUILDING);
	bool ret;

	if (!state || state->task != current ||
	    state->phase != UINTR_SIGNAL_MPK_BUILDING ||
	    pkru != state->expected_pkru) {
		ret = orig_copy_fpstate_to_sigframe(buf, buf_fx, size, pkru);
		goto out;
	}

	/*
	 * Stable kernels with only the four-argument PKRU plumbing still run
	 * XSAVE with the interrupted PKRU. Open pkey 0 while Linux writes the
	 * protected host frame, then explicitly preserve the interrupted value
	 * in its user-format xstate image.
	 */
	write_pkru(FLUX_MPK_KERNEL_PKRU);
	ret = orig_copy_fpstate_to_sigframe(buf, buf_fx, size, pkru);
	if (ret && flux_mpk_patch_signal_pkru(buf_fx, size, pkru)) {
		state->fpstate = buf_fx;
		state->fpstate_size = size;
		ret = true;
		goto out;
	}

	write_pkru(state->expected_pkru);
	ret = false;
out:
	flux_mpk_call_done();
	return ret;
}

static int hook_x64_setup_rt_frame(struct ksignal *ksig, struct pt_regs *regs)
{
	struct uintr_percpu *p = flux_mpk_uintr_this_cpu();
	struct uintr_signal_record *state;
	struct rt_sigframe __user *frame;
	stack_t saved_stack;
	u64 signal_cookie;
	bool captured_uif = false;
	unsigned long host_fsbase = 0;
	bool flux_frame;
	unsigned long stack_start;
	unsigned long stack_size;
	unsigned int slot;
	u32 pkru = read_pkru();
	unsigned long handler = (unsigned long)ksig->ka.sa.sa_handler;
	int logical_cpu = -1;
	int ret;

	/*
	 * This hook is global.  A non-Flux task may build a signal frame after a
	 * Flux worker captured UIF on the same CPU; it must not consume that
	 * worker's pending frame transaction.
	 */
	if (!flux_mpk_worker_current()) {
		ret = orig_x64_setup_rt_frame(ksig, regs);
		goto out_done;
	}

	if (pkru != FLUX_MPK_APP_PKRU && pkru != FLUX_MPK_KERNEL_PKRU) {
		ret = orig_x64_setup_rt_frame(ksig, regs);
		goto out;
	}

	if (!(ksig->ka.sa.sa_flags & SA_ONSTACK)) {
		ret = -EFAULT;
		goto out;
	}
	state = flux_mpk_signal_state_begin(&ret);
	if (!state)
		goto out;
	if (WARN_ON_ONCE(!p->assigned_ctx || !p->assigned_ctx->signal_stack ||
			 p->assigned_ctx->signal_stack_slot_size == 0)) {
		ret = -EFAULT;
		goto out;
	}
	slot = state - p->sig_records;
	stack_size = p->assigned_ctx->signal_stack_slot_size;
	stack_start = p->assigned_ctx->signal_stack + slot * stack_size;
	if (handler != p->assigned_ctx->signal_handler) {
		ret = -EFAULT;
		goto out;
	}

	state->task = current;
	state->phase = UINTR_SIGNAL_MPK_BUILDING;
	state->expected_pkru = pkru;
	state->stack_start = stack_start;
	state->stack_size = stack_size;

	saved_stack.ss_sp = (void __user *)current->sas_ss_sp;
	saved_stack.ss_size = current->sas_ss_size;
	saved_stack.ss_flags = current->sas_ss_flags;
	current->sas_ss_sp = stack_start;
	current->sas_ss_size = stack_size;
	current->sas_ss_flags = SS_AUTODISARM;
	ret = orig_x64_setup_rt_frame(ksig, regs);
	current->sas_ss_sp = (unsigned long)saved_stack.ss_sp;
	current->sas_ss_size = saved_stack.ss_size;
	current->sas_ss_flags = saved_stack.ss_flags;
	if (ret)
		goto frame_failed;

	frame = (struct rt_sigframe __user *)regs->sp;
	if (copy_to_user(&frame->uc.uc_stack, &saved_stack,
			 sizeof(saved_stack))) {
		ret = -EFAULT;
		goto frame_failed;
	}
	if (ret || read_pkru() != FLUX_MPK_KERNEL_PKRU || !state->fpstate ||
	    state->fpstate_size == 0 ||
	    !flux_mpk_range_within(regs->sp, sizeof(struct rt_sigframe),
				   state->stack_start, state->stack_size) ||
	    !flux_mpk_range_within((unsigned long)state->fpstate,
				   state->fpstate_size, state->stack_start,
				   state->stack_size)) {
		ret = ret ?: -EFAULT;
		goto frame_failed;
	}

	state->frame = regs->sp;
	state->phase = UINTR_SIGNAL_FRAME_READY;
	/* Private fourth argument consumed by flux_host_signal_entry. */
	regs->cx = pkru;
	ret = 0;
	goto out;
frame_failed:
	write_pkru(pkru);
out:
	flux_frame = uintr_complete_signal_frame(ret ? 0 : regs->sp,
						  &captured_uif, &logical_cpu,
						  &host_fsbase);
	if (!ret && flux_frame) {
		/*
		 * Native signal setup leaves caller-saved R8/R9 untouched.  Tag the
		 * private fifth argument so a non-Flux interrupted register image can
		 * never be mistaken for delivery metadata.  A missing frame owner or
		 * overflow remains tagged but invalid and therefore fails closed.
		 */
		signal_cookie = FLUX_SIGNAL_ENTRY_COOKIE_TAG;
		if (logical_cpu >= 0 &&
		    (unsigned int)logical_cpu <=
			    (unsigned int)FLUX_SIGNAL_ENTRY_COOKIE_CPU_MASK) {
			signal_cookie |= FLUX_SIGNAL_ENTRY_COOKIE_VALID |
					 (u64)logical_cpu;
			if (captured_uif)
				signal_cookie |= FLUX_SIGNAL_ENTRY_COOKIE_UIF;
		}
		regs->r8 = signal_cookie;
		regs->r9 = host_fsbase;
	}
out_done:
	flux_mpk_call_done();
	return ret;
}

static void hook_fpu_clear_user_states(struct fpu *fpu)
{
	struct uintr_signal_record *state =
		flux_mpk_signal_state_find_phase(UINTR_SIGNAL_FRAME_READY);
	bool signal_frame = state && state->task == current &&
			    state->phase == UINTR_SIGNAL_FRAME_READY;

	orig_fpu_clear_user_states(fpu);
	if (!signal_frame) {
		flux_mpk_call_done();
		return;
	}

	uintr_reclear_signal_uif();
	write_pkru(FLUX_MPK_KERNEL_PKRU);
	state->phase = UINTR_SIGNAL_ACTIVE;
	flux_mpk_call_done();
}

int flux_mpk_enable(struct flux_mm_ctx *ctx)
{
	int ret = 0;

	mutex_lock(&flux_mpk_hook_lock);
	if (ctx->mpk_enabled)
		goto out;

	if (!flux_mpk_hooks_installed) {
		ret = flux_mpk_state_init();
		if (ret)
			goto out;

		if (!try_module_get(THIS_MODULE)) {
			ret = -ENODEV;
			goto out;
		}
		flux_mpk_module_ref = true;
		ret = flux_hook_group_install(&mpk_hook_group);
		if (ret) {
			if (flux_hook_group_active(&mpk_hook_group)) {
				flux_mpk_hooks_installed = true;
				flux_mpk_schedule_disable_locked();
			} else {
				flux_mpk_module_ref = false;
				module_put(THIS_MODULE);
			}
			goto out;
		}
		flux_mpk_hooks_installed = true;
	}

	WRITE_ONCE(ctx->mpk_enabled, true);
	flux_mpk_users++;
out:
	mutex_unlock(&flux_mpk_hook_lock);
	return ret;
}

void flux_mpk_ctx_release(struct flux_mm_ctx *ctx)
{
	mutex_lock(&flux_mpk_hook_lock);
	if (!ctx->mpk_enabled)
		goto out;

	WRITE_ONCE(ctx->mpk_enabled, false);
	if (WARN_ON(!flux_mpk_users))
		goto out;

	flux_mpk_users--;
	if (!flux_mpk_users)
		flux_mpk_schedule_disable_locked();
out:
	mutex_unlock(&flux_mpk_hook_lock);
}

int flux_mpk_validate_app_range(struct flux_mm_ctx *ctx, unsigned long arg)
{
	struct flux_mpk_range range;
	struct vm_area_struct *vma;
	unsigned long cursor;
	unsigned long end;
	bool alias_only = true;
	bool shadow_only = true;
	bool saw_shadow = false;
	int ret = 0;

	if (!READ_ONCE(ctx->mpk_enabled))
		return -EACCES;
	if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
		return -EFAULT;
	if (!range.len)
		return 0;
	if (check_add_overflow(range.start, range.len, &end))
		return -EINVAL;
	if (!current->mm)
		return -EINVAL;

	/*
	 * Alias installation holds this mutex across its temporary PROT_NONE
	 * reservation and PFNMAP/PTE conversion.  Take it before mmap_lock, in
	 * the same order as the alias paths, so validation cannot reject that
	 * safe intermediate VMA as a non-app mapping.
	 */
	mutex_lock(flux_alias_mm_lock(current->mm));
	cursor = range.start;
	mmap_read_lock(current->mm);
	vma = find_vma_intersection(current->mm, range.start, end);
	while (vma && vma->vm_start < end) {
		unsigned long first = max(range.start, vma->vm_start);
		unsigned long last = min(end, vma->vm_end);

		if (first != cursor) {
			alias_only = false;
		}
		if (vma_pkey(vma) == FLUX_MPK_APP_PKEY) {
			alias_only = false;
			if (vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))
				shadow_only = false;
			else
				saw_shadow = true;
		} else {
			if (!flux_alias_range_has_pkey(vma, first, last,
						      FLUX_MPK_APP_PKEY)) {
				ret = -EPERM;
				break;
			}
			saw_shadow = true;
		}
		cursor = last;
		vma = find_vma(current->mm, vma->vm_end);
	}
	/*
	 * Distinguish a completed alias from a safe host shadow.  A shadow may
	 * cover only part of the requested range; gaps can be filled when the
	 * caller rebuilds the reservation.  Accessible app VMAs retain the
	 * validate-only zero result, and non-app mappings remain errors.
	 */
	if (!ret && alias_only && cursor == end)
		ret = FLUX_MPK_APP_RANGE_ALIAS;
	else if (!ret && shadow_only && saw_shadow)
		ret = FLUX_MPK_APP_RANGE_SHADOW;
	mmap_read_unlock(current->mm);
	mutex_unlock(flux_alias_mm_lock(current->mm));

	return ret;
}

static struct flux_ftrace_hook mpk_hooks[] = {
	FLUX_SYSCALL_HOOK("x64_sys_call", hook_x64_sys_call, &orig_x64_sys_call,
			  &flux_mpk_active_calls),
	FLUX_TRACKED_HOOK(
		"copy_fpstate_to_sigframe", hook_copy_fpstate_to_sigframe,
		&orig_copy_fpstate_to_sigframe, &flux_mpk_active_calls),
	FLUX_TRACKED_HOOK("fpu__clear_user_states", hook_fpu_clear_user_states,
			  &orig_fpu_clear_user_states, &flux_mpk_active_calls),
};

static struct flux_hook_group mpk_hook_group = {
	.name = "mpk",
	.hooks = mpk_hooks,
	.nr_hooks = ARRAY_SIZE(mpk_hooks),
};

static struct flux_ftrace_hook uintr_signal_hooks[] = {
	FLUX_GLOBAL_TRACKED_HOOK("__x64_sys_rt_sigreturn",
				 hook_x64_sys_rt_sigreturn,
				 &orig_x64_sys_rt_sigreturn,
				 &flux_mpk_active_calls),
	FLUX_GLOBAL_TRACKED_HOOK("x64_setup_rt_frame",
				 hook_x64_setup_rt_frame,
				 &orig_x64_setup_rt_frame,
				 &flux_mpk_active_calls),
};

static struct flux_hook_group uintr_signal_hook_group = {
	.name = "uintr_signal",
	.hooks = uintr_signal_hooks,
	.nr_hooks = ARRAY_SIZE(uintr_signal_hooks),
};

static int flux_mpk_disable_thread(void *unused)
{
	int err = 0;
	bool put_module = false;

	mutex_lock(&flux_mpk_hook_lock);
	if (!flux_mpk_users && flux_mpk_hooks_installed) {
		err = flux_hook_group_remove(&mpk_hook_group);
		if (!err) {
			wait_event(flux_mpk_active_waitq,
				   atomic_read(&flux_mpk_active_calls) == 0);
			synchronize_rcu_tasks();
			flux_mpk_hooks_installed = false;
			if (flux_mpk_module_ref) {
				flux_mpk_module_ref = false;
				put_module = true;
			}
		}
	}
	flux_mpk_disable_pending = false;
	mutex_unlock(&flux_mpk_hook_lock);
	if (put_module)
		module_put_and_kthread_exit(0);
	return err;
}

static int flux_mpk_state_init(void)
{
	unsigned int eax = 0x0d;
	unsigned int ebx = 0;
	unsigned int ecx = XFEATURE_PKRU;
	unsigned int edx = 0;

	native_cpuid(&eax, &ebx, &ecx, &edx);
	if (eax < sizeof(struct pkru_state) || !ebx)
		return -EOPNOTSUPP;
	flux_mpk_pkru_xstate_offset = ebx;

	return 0;
}

int flux_uintr_signal_hook_init(void)
{
	int ret = 0;

	mutex_lock(&flux_mpk_hook_lock);
	if (flux_uintr_signal_hooks_installed)
		goto out;
	ret = flux_hook_group_install(&uintr_signal_hook_group);
	if (!ret)
		flux_uintr_signal_hooks_installed = true;
out:
	mutex_unlock(&flux_mpk_hook_lock);
	return ret;
}

void flux_uintr_signal_hook_exit(void)
{
	mutex_lock(&flux_mpk_hook_lock);
	if (flux_uintr_signal_hooks_installed &&
	    !flux_hook_group_remove(&uintr_signal_hook_group)) {
		wait_event(flux_mpk_active_waitq,
			   atomic_read(&flux_mpk_active_calls) == 0);
		synchronize_rcu_tasks();
		flux_uintr_signal_hooks_installed = false;
	}
	mutex_unlock(&flux_mpk_hook_lock);
}

void flux_mpk_hook_exit(void)
{
	mutex_lock(&flux_mpk_hook_lock);
	WARN_ON(flux_mpk_users || flux_mpk_disable_pending ||
		flux_mpk_module_ref);
	if (flux_mpk_hooks_installed) {
		if (!flux_hook_group_remove(&mpk_hook_group)) {
			wait_event(flux_mpk_active_waitq,
				   atomic_read(&flux_mpk_active_calls) == 0);
			synchronize_rcu_tasks();
			flux_mpk_hooks_installed = false;
		}
	}
	mutex_unlock(&flux_mpk_hook_lock);
}
