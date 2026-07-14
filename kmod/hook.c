#define pr_fmt(fmt) "flux_hook: " fmt

#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/version.h>
#include <asm/unistd.h>

#include "hook.h"
#include "mm.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t flux_kallsyms_lookup_name;
#endif

int flux_hook_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int err;

	err = register_kprobe(&kp);
	if (err)
		return err;
	flux_kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	if (!flux_kallsyms_lookup_name)
		return -ENXIO;
#endif
	return 0;
}

unsigned long flux_lookup_symbol(const char *name)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	return flux_kallsyms_lookup_name ? flux_kallsyms_lookup_name(name) : 0;
#else
	return kallsyms_lookup_name(name);
#endif
}

static int flux_resolve_hook(struct flux_ftrace_hook *hook)
{
	if (hook->address)
		return -EEXIST;

	hook->address = flux_lookup_symbol(hook->name);
	if (!hook->address) {
		pr_err("unresolved symbol: %s\n", hook->name);
		return -ENOENT;
	}

	*(unsigned long *)hook->orig = hook->address;
	return 0;
}

static void notrace flux_ftrace_thunk(unsigned long ip,
				      unsigned long parent_ip,
				      struct ftrace_ops *ops,
				      struct ftrace_regs *regs)
{
	struct flux_ftrace_hook *hook =
		container_of(ops, struct flux_ftrace_hook, ops);
	struct pt_regs *regs_ptr = ftrace_get_regs(regs);

	if (hook->bypass_nonreturning_syscalls &&
	    (regs_ptr->si == __NR_delete_module ||
	     regs_ptr->si == __NR_exit ||
	     regs_ptr->si == __NR_exit_group))
		return;

	if (!within_module(parent_ip, THIS_MODULE)) {
		if (hook->mpk_context_only &&
		    !flux_mm_mpk_enabled_current_rcu())
			return;
		if (hook->active_calls)
			atomic_inc(hook->active_calls);
		regs_ptr->ip = (unsigned long)hook->func;
	}
}

static int flux_install_hook(struct flux_ftrace_hook *hook)
{
	int err;

	err = flux_resolve_hook(hook);
	if (err)
		return err;

	hook->ops.func = flux_ftrace_thunk;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION |
			  FTRACE_OPS_FL_IPMODIFY;
	err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 1);
	if (err)
		goto out_clear;

	err = register_ftrace_function(&hook->ops);
	if (err)
		goto out_filter;

	pr_info("hook %s at %px\n", hook->name, (void *)hook->address);
	return 0;

out_filter:
	ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
out_clear:
	hook->address = 0;
	return err;
}

static int flux_remove_hook(struct flux_ftrace_hook *hook)
{
	int err;

	if (!hook->address)
		return 0;

	err = unregister_ftrace_function(&hook->ops);
	if (err) {
		pr_err("failed to unregister hook %s: %d\n", hook->name, err);
		return err;
	}
	ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
	pr_info("remove hook from %s\n", hook->name);
	hook->address = 0;
	return 0;
}

int flux_hook_group_install(struct flux_hook_group *group)
{
	int i, err;

	for (i = 0; i < group->nr_hooks; i++) {
		err = flux_install_hook(&group->hooks[i]);
		if (err) {
			while (--i >= 0)
				flux_remove_hook(&group->hooks[i]);
			return err;
		}
	}
	return 0;
}

int flux_hook_group_remove(struct flux_hook_group *group)
{
	int i, err;
	int first_err = 0;

	for (i = 0; i < group->nr_hooks; i++) {
		err = flux_remove_hook(&group->hooks[i]);
		if (err && !first_err)
			first_err = err;
	}
	return first_err;
}

bool flux_hook_group_active(struct flux_hook_group *group)
{
	int i;

	for (i = 0; i < group->nr_hooks; i++) {
		if (group->hooks[i].address)
			return true;
	}
	return false;
}
