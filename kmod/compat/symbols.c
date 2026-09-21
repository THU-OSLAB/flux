// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/version.h>

#include "symbols.h"

/* kallsyms_lookup_name stopped being exported to modules in Linux 5.7. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
typedef unsigned long (*flux_lookup_name_fn)(const char *name);
static flux_lookup_name_fn flux_kallsyms_lookup_name;
#endif

int flux_compat_symbols_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int err;

	err = register_kprobe(&kp);
	if (err)
		return err;
	flux_kallsyms_lookup_name = (flux_lookup_name_fn)kp.addr;
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
