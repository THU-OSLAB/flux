#define pr_fmt(fmt) "flux_mpk: " fmt

#include <asm/cpuid.h>
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

static long (*orig_x64_sys_call)(const struct pt_regs *regs,
				  unsigned int nr);
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
static bool flux_mpk_disable_pending;
static bool flux_mpk_module_ref;

static struct flux_hook_group mpk_hook_group;
static int flux_mpk_disable_thread(void *unused);

static void flux_mpk_schedule_disable_locked(void)
{
	struct task_struct *task;

	if (flux_mpk_disable_pending)
		return;

	flux_mpk_disable_pending = true;
	task = kthread_run(flux_mpk_disable_thread, NULL,
			   "flux_mpk_cleanup");
	if (IS_ERR(task)) {
		flux_mpk_disable_pending = false;
		pr_err("failed to start cleanup thread: %ld\n", PTR_ERR(task));
	}
}

static bool flux_mpk_patch_signal_pkru(void __user *buf_fx, int size,
				       u32 pkru);

static void flux_mpk_call_done(void)
{
	if (atomic_dec_and_test(&flux_mpk_active_calls))
		wake_up_all(&flux_mpk_active_waitq);
}

enum flux_mpk_signal_phase {
	FLUX_MPK_SIGNAL_IDLE,
	FLUX_MPK_SIGNAL_BUILDING,
	FLUX_MPK_SIGNAL_FRAME_READY,
	FLUX_MPK_SIGNAL_ACTIVE,
};

struct flux_mpk_signal_state {
	struct task_struct *task;
	enum flux_mpk_signal_phase phase;
	u32 expected_pkru;
	unsigned long stack_start;
	unsigned long stack_size;
	unsigned long frame;
	void __user *fpstate;
	unsigned int fpstate_size;
};

static DEFINE_PER_CPU(struct flux_mpk_signal_state, flux_mpk_signal_state);

static struct flux_mpk_signal_state *flux_mpk_signal_state_this_cpu(void)
{
	return raw_cpu_ptr(&flux_mpk_signal_state);
}

static void flux_mpk_signal_state_reset(struct flux_mpk_signal_state *state)
{
	memset(state, 0, sizeof(*state));
}

static bool flux_mpk_worker_current(void)
{
	struct flux_percpu *pcpu = raw_cpu_ptr(&flux_percpu);

	return READ_ONCE(pcpu->uintr.assigned_task) == current;
}

static bool flux_mpk_range_has_pkey(unsigned long start, unsigned long len,
				    int pkey, vm_flags_t required_flags)
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
	struct flux_mm_ctx *ctx = flux_mm_ctx_get_current();
	bool enabled = false;

	if (ctx)
		enabled = READ_ONCE(ctx->mpk_enabled);
	flux_mm_ctx_put(ctx);

	return enabled;
}

static bool flux_mpk_restricted_current(void)
{
	return flux_mpk_enabled_current() &&
	       (read_pkru() & FLUX_MPK_PROTECTED_MASK);
}

static bool flux_mpk_prepare_sigreturn(const struct pt_regs *regs,
				       struct flux_mpk_signal_state *state)
{
	struct xregs_state __user *xsave = state->fpstate;
	u64 xfeatures, xcomp_bv;

	if (state->phase != FLUX_MPK_SIGNAL_ACTIVE || state->task != current ||
	    read_pkru() != FLUX_MPK_KERNEL_PKRU ||
	    regs->sp != state->frame + sizeof(unsigned long) ||
	    !flux_mpk_range_within(state->frame, sizeof(struct rt_sigframe),
				   state->stack_start, state->stack_size) ||
	    !flux_mpk_range_within((unsigned long)state->fpstate,
				   state->fpstate_size, state->stack_start,
				   state->stack_size) ||
	    !flux_mpk_range_has_pkey(state->frame, sizeof(struct rt_sigframe),
				     FLUX_MPK_KERNEL_PKEY, VM_WRITE) ||
	    !flux_mpk_range_has_pkey((unsigned long)state->fpstate,
				     state->fpstate_size,
				     FLUX_MPK_KERNEL_PKEY, VM_WRITE))
		return false;

	if (__get_user(xfeatures, &xsave->header.xfeatures) ||
	    __get_user(xcomp_bv, &xsave->header.xcomp_bv) ||
	    !(xfeatures & XFEATURE_MASK_PKRU) ||
	    (xcomp_bv & XCOMP_BV_COMPACTED_FORMAT))
		return false;

	return flux_mpk_patch_signal_pkru(state->fpstate,
					  state->fpstate_size,
					  state->expected_pkru);
}

static long hook_x64_sys_call(const struct pt_regs *regs, unsigned int nr)
{
	struct flux_mpk_signal_state *state =
		flux_mpk_signal_state_this_cpu();
	long ret;

	if (nr == __NR_rt_sigreturn && state->task == current &&
	    state->phase == FLUX_MPK_SIGNAL_ACTIVE) {
		if (!flux_mpk_prepare_sigreturn(regs, state)) {
			flux_mpk_signal_state_reset(state);
			ret = -EPERM;
			goto out;
		}

		ret = orig_x64_sys_call(regs, nr);
		flux_mpk_signal_state_reset(state);
		goto out;
	}

	if (unlikely(flux_mpk_restricted_current())) {
		ret = -EPERM;
		goto out;
	}

	ret = orig_x64_sys_call(regs, nr);
out:
	flux_mpk_call_done();
	return ret;
}

static bool flux_mpk_patch_signal_pkru(void __user *buf_fx, int size,
				       u32 pkru)
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
	if (copy_to_user((char __user *)buf_fx +
			 flux_mpk_pkru_xstate_offset,
			 &pkru_state, sizeof(pkru_state)))
		return false;

	return true;
}

static bool hook_copy_fpstate_to_sigframe(void __user *buf,
					  void __user *buf_fx, int size,
					  u32 pkru)
{
	struct flux_mpk_signal_state *state =
		flux_mpk_signal_state_this_cpu();
	bool ret;

	if (state->task != current ||
	    state->phase != FLUX_MPK_SIGNAL_BUILDING ||
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

static int hook_x64_setup_rt_frame(struct ksignal *ksig,
				   struct pt_regs *regs)
{
	struct flux_mpk_signal_state *state =
		flux_mpk_signal_state_this_cpu();
	u32 pkru = read_pkru();
	unsigned long handler = (unsigned long)ksig->ka.sa.sa_handler;
	int ret;

	if (!flux_mpk_worker_current() ||
	    (pkru != FLUX_MPK_APP_PKRU && pkru != FLUX_MPK_KERNEL_PKRU)) {
		ret = orig_x64_setup_rt_frame(ksig, regs);
		goto out;
	}

	if (state->phase != FLUX_MPK_SIGNAL_IDLE ||
	    !(ksig->ka.sa.sa_flags & SA_ONSTACK) || !current->sas_ss_size ||
	    !flux_mpk_range_has_pkey(current->sas_ss_sp, current->sas_ss_size,
				     FLUX_MPK_KERNEL_PKEY, VM_WRITE) ||
	    !flux_mpk_range_has_pkey(handler, 1, FLUX_MPK_KERNEL_PKEY,
				     VM_EXEC))
		{
			ret = -EFAULT;
			goto out;
		}

	state->task = current;
	state->phase = FLUX_MPK_SIGNAL_BUILDING;
	state->expected_pkru = pkru;
	state->stack_start = current->sas_ss_sp;
	state->stack_size = current->sas_ss_size;

	ret = orig_x64_setup_rt_frame(ksig, regs);
	if (ret || read_pkru() != FLUX_MPK_KERNEL_PKRU || !state->fpstate ||
	    state->fpstate_size == 0 ||
	    !flux_mpk_range_within(regs->sp, sizeof(struct rt_sigframe),
				   state->stack_start, state->stack_size) ||
	    !flux_mpk_range_within((unsigned long)state->fpstate,
				   state->fpstate_size, state->stack_start,
				   state->stack_size)) {
		write_pkru(pkru);
		flux_mpk_signal_state_reset(state);
		ret = ret ?: -EFAULT;
		goto out;
	}

	state->frame = regs->sp;
	state->phase = FLUX_MPK_SIGNAL_FRAME_READY;
	/* Private fourth argument consumed by flux_host_signal_entry. */
	regs->cx = pkru;
	ret = 0;
out:
	flux_mpk_call_done();
	return ret;
}

static void hook_fpu_clear_user_states(struct fpu *fpu)
{
	struct flux_mpk_signal_state *state =
		flux_mpk_signal_state_this_cpu();
	bool signal_frame = state->task == current &&
			    state->phase == FLUX_MPK_SIGNAL_FRAME_READY;

	orig_fpu_clear_user_states(fpu);
	if (!signal_frame) {
		flux_mpk_call_done();
		return;
	}

	write_pkru(FLUX_MPK_KERNEL_PKRU);
	state->phase = FLUX_MPK_SIGNAL_ACTIVE;
	flux_mpk_call_done();
}

int flux_mpk_enable(struct flux_mm_ctx *ctx)
{
	int ret = 0;

	mutex_lock(&flux_mpk_hook_lock);
	if (ctx->mpk_enabled)
		goto out;

	if (!flux_mpk_hooks_installed) {
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
	unsigned long end;
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

	mmap_read_lock(current->mm);
	vma = find_vma_intersection(current->mm, range.start, end);
	while (vma && vma->vm_start < end) {
		if (vma_pkey(vma) != FLUX_MPK_APP_PKEY) {
			ret = -EPERM;
			break;
		}
		vma = find_vma(current->mm, vma->vm_end);
	}
	mmap_read_unlock(current->mm);

	return ret;
}

static struct flux_ftrace_hook mpk_hooks[] = {
	FLUX_SYSCALL_HOOK("x64_sys_call", hook_x64_sys_call,
			  &orig_x64_sys_call, &flux_mpk_active_calls),
	FLUX_TRACKED_HOOK("x64_setup_rt_frame", hook_x64_setup_rt_frame,
			  &orig_x64_setup_rt_frame, &flux_mpk_active_calls),
	FLUX_TRACKED_HOOK("copy_fpstate_to_sigframe",
			  hook_copy_fpstate_to_sigframe,
			  &orig_copy_fpstate_to_sigframe,
			  &flux_mpk_active_calls),
	FLUX_TRACKED_HOOK("fpu__clear_user_states",
			  hook_fpu_clear_user_states,
			  &orig_fpu_clear_user_states,
			  &flux_mpk_active_calls),
};

static struct flux_hook_group mpk_hook_group = {
	.name = "mpk",
	.hooks = mpk_hooks,
	.nr_hooks = ARRAY_SIZE(mpk_hooks),
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

int flux_mpk_hook_init(void)
{
	unsigned int eax, ebx, ecx, edx;

	cpuid_count(0x0d, XFEATURE_PKRU, &eax, &ebx, &ecx, &edx);
	if (eax < sizeof(struct pkru_state) || !ebx)
		return -EOPNOTSUPP;
	flux_mpk_pkru_xstate_offset = ebx;

	return 0;
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
