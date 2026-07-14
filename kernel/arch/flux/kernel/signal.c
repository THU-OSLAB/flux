#define pr_fmt(fmt) "signal: " fmt

#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/signal.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/irqflags.h>
#include <linux/pid.h>
#include <linux/kernel.h>
#include <asm/unistd.h>
#include <asm/host_dev.h>
#include <asm/host_ops.h>
#include <asm/signal.h>
#include <asm/x86/current.h>
#include <uapi/asm-generic/ucontext.h>
#include <uapi/asm/flux_ops.h>
#include <uapi/asm/flux_oci.h>
#include <uapi/asm/mpk.h>

#ifdef CONFIG_FLUX_UINTR

DEFINE_PER_CPU(struct flux_sig_list, flux_sig_lists);
static bool flux_sig_initialized = false;
static struct pid *flux_init_pid;
static struct pid *flux_sig_get_init_pid(void);

static __init int flux_sig_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct flux_sig_list *list = &per_cpu(flux_sig_lists, cpu);

		spin_lock_init(&list->lock);
		INIT_LIST_HEAD(&list->head);
	}

	flux_sig_initialized = true;

	pr_info("signal lists initialized\n");

	return 0;
}
late_initcall(flux_sig_init);

static int flux_sig_cpu_from_tid(long host_tid)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (READ_ONCE(per_cpu(tls_pcpu.host_tid, cpu)) == host_tid)
			return cpu;
	}

	return -1;
}

#define FLUX_UINTR_REDZONE 128
#define FLUX_UINTR_SW_FRAME_QWORDS 4

static inline bool flux_sig_prepare_uintr_frame(void *ucontext,
					 unsigned int interrupted_pkru)
{
	struct ucontext *uc = ucontext;
	struct sigcontext *sc;
	unsigned long orig_sp, frame_sp;
	u64 *frame;

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
	orig_sp = sc->sp;
	frame_sp = round_down(orig_sp - FLUX_UINTR_REDZONE -
				      FLUX_UINTR_SW_FRAME_QWORDS * sizeof(u64),
			      16UL);
	frame = (u64 *)frame_sp;

#ifdef CONFIG_FLUX_MPK
	if (frame_sp > orig_sp)
		return false;
	if (interrupted_pkru == FLUX_MPK_APP_PKRU &&
	    flux_host_dev_validate_app_range(
		frame_sp, FLUX_UINTR_SW_FRAME_QWORDS * sizeof(u64)))
		return false;
	if (interrupted_pkru != FLUX_MPK_APP_PKRU &&
	    interrupted_pkru != FLUX_MPK_KERNEL_PKRU)
		return false;
#endif

	frame[0] = FLUX_UINTR_VECTOR_SIGNAL;
	frame[1] = sc->ip;
	frame[2] = sc->flags;
	frame[3] = orig_sp;

	sc->sp = frame_sp;
	sc->ip = (unsigned long)flux_uintr_handler;
	return true;
}

struct flux_sig_entry *flux_sig_take_entry(int cpu)
{
	struct flux_sig_list *list;
	struct flux_sig_entry *entry = NULL;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return NULL;

	list = &per_cpu(flux_sig_lists, cpu);
	spin_lock(&list->lock);
	if (!list_empty(&list->head)) {
		entry = list_first_entry(&list->head, struct flux_sig_entry,
					 sig_link);
		list_del_init(&entry->sig_link);
	}
	spin_unlock(&list->lock);

	return entry;
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

static bool flux_sig_parse_control_signal(int sig, const siginfo_t *si,
					  struct flux_sig_entry *entry)
{
	unsigned int packed;
	unsigned int op;
	unsigned int signo;

	if (!si || sig != SIGUSR1 || si->si_code != SI_QUEUE)
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
		entry->sig_pid = flux_sig_get_init_pid();
		return true;
#ifdef CONFIG_FLUX_RUNC
	case FLUX_SIGNAL_CTRL_EXEC:
		entry->sig_nr = 0;
		entry->sig_code = SI_QUEUE;
		entry->sig_addr = 0;
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

void flux_signal_handler(int sig, void *info, void *ucontext,
			 unsigned int interrupted_pkru)
{
	int flux_cpu;
	long host_tid;
	struct flux_sig_list *list;
	struct flux_sig_entry *entry;
	struct task_struct *target;
	siginfo_t *si = info;

	if (!valid_signal(sig) || !flux_sig_initialized)
		return;

	WARN_ON(!arch_irqs_disabled());

	host_tid = flux_ops_gettid_raw();
	flux_cpu = flux_sig_cpu_from_tid(host_tid);
	if (flux_cpu < 0) {
		pr_warn_ratelimited(
			"recovered signal %d for unknown host tid %ld on CPU %d\n",
			sig, host_tid, flux_cpu);
		return;
	}

	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return;

	entry->sig_nr = sig;
	entry->ctrl_op = FLUX_SIGNAL_CTRL_NONE;
	entry->ctrl_arg = 0;
	entry->sig_code = 0;
	entry->sig_addr = 0;
	entry->sig_pid = NULL;

	if (flux_sig_parse_control_signal(sig, si, entry))
		goto out_queue;

	if (si && si->si_signo == sig) {
		entry->sig_code = si->si_code;
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

	if (flux_sig_is_sync_fault(sig, si))
		entry->sig_pid = flux_sig_get_current_task_pid(flux_cpu);
	else
		entry->sig_pid = flux_sig_get_init_pid();

	target = per_cpu(tls_pcpu.current_task, flux_cpu);
	if (target && !entry->sig_pid)
		entry->sig_pid = get_task_pid(target, PIDTYPE_PID);

out_queue:
	if (!flux_sig_prepare_uintr_frame(ucontext, interrupted_pkru)) {
		if (entry->sig_pid)
			put_pid(entry->sig_pid);
		kfree(entry);
		return;
	}

	INIT_LIST_HEAD(&entry->sig_link);
	list = &per_cpu(flux_sig_lists, flux_cpu);
	spin_lock(&list->lock);
	list_add_tail(&entry->sig_link, &list->head);
	spin_unlock(&list->lock);

}

#else

void flux_signal_handler(int sig, void *info, void *ucontext,
			 unsigned int interrupted_pkru)
{
}

#endif /* CONFIG_FLUX_UINTR */
