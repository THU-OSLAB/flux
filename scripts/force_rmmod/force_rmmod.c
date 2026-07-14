#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/cpumask.h>
#include <linux/list.h>
#include <asm-generic/local.h>
#include <linux/platform_device.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>

static char *modname = NULL;
module_param(modname, charp, 0644);
MODULE_PARM_DESC(modname,
		 "The name of module you want do clean or delete...\n");

static int force_cleanup_module(char *del_mod_name)
{
	struct module *mod = NULL, *relate = NULL;
	int cpu;

	struct module *list_mod = NULL;
	list_for_each_entry(list_mod, THIS_MODULE->list.prev, list) {
		if (strcmp(list_mod->name, del_mod_name) == 0) {
			mod = list_mod;
		}
	}

	if (mod == NULL) {
		printk("[%s] module %s not found\n", THIS_MODULE->name,
		       modname);
		return -1;
	}

	if (!list_empty(&mod->source_list)) {
		list_for_each_entry(relate, &mod->source_list, source_list) {
			printk("[relate]:%s\n", relate->name);
		}
	} else {
		printk("No modules depend on %s...\n", del_mod_name);
	}

	mod->state = MODULE_STATE_LIVE;
	for_each_possible_cpu(cpu) {
		local_set((local_t *)per_cpu_ptr(&(mod->refcnt), cpu), 0);
	}
	atomic_set(&mod->refcnt, 1);

	printk("[after] name:%s, state:%d, refcnt:%u\n", mod->name, mod->state,
	       module_refcount(mod));

	return 0;
}

static int __init force_rmmod_init(void)
{
	return force_cleanup_module(modname);
}

static void __exit force_rmmod_exit(void)
{
	printk("=======name : %s, state : %d EXIT=======\n", THIS_MODULE->name,
	       THIS_MODULE->state);
}

module_init(force_rmmod_init);
module_exit(force_rmmod_exit);

MODULE_LICENSE("GPL");