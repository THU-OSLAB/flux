#ifndef _FLUX_KMOD_HOOK_H
#define _FLUX_KMOD_HOOK_H

#include <linux/atomic.h>
#include <linux/ftrace.h>

#include "compat/symbols.h"

#define FLUX_HOOK(_name, _func, _orig) \
	{                                 \
		.name = (_name),            \
		.func = (_func),            \
		.orig = (_orig),            \
		.address = 0,               \
	}

#define FLUX_TRACKED_HOOK(_name, _func, _orig, _active_calls) \
	{                                                        \
		.name = (_name),                                   \
		.func = (_func),                                   \
		.orig = (_orig),                                   \
		.address = 0,                                      \
		.active_calls = (_active_calls),                    \
		.mpk_context_only = true,                          \
	}

#define FLUX_GLOBAL_TRACKED_HOOK(_name, _func, _orig, _active_calls) \
	{                                                               \
		.name = (_name),                                         \
		.func = (_func),                                         \
		.orig = (_orig),                                         \
		.address = 0,                                            \
		.active_calls = (_active_calls),                          \
	}

#define FLUX_SYSCALL_HOOK(_name, _func, _orig, _active_calls) \
	{                                                       \
		.name = (_name),                                  \
		.func = (_func),                                  \
		.orig = (_orig),                                  \
		.address = 0,                                     \
		.active_calls = (_active_calls),                   \
		.bypass_nonreturning_syscalls = true,             \
		.mpk_context_only = true,                         \
	}

struct flux_ftrace_hook {
	const char *name;
	void *func;
	void *orig;
	unsigned long address;
	atomic_t *active_calls;
	bool bypass_nonreturning_syscalls;
	bool mpk_context_only;
	struct ftrace_ops ops;
};

struct flux_hook_group {
	const char *name;
	struct flux_ftrace_hook *hooks;
	size_t nr_hooks;
};

extern int flux_hook_init(void);
extern int flux_hook_group_install(struct flux_hook_group *group);
extern int flux_hook_group_remove(struct flux_hook_group *group);
extern bool flux_hook_group_active(struct flux_hook_group *group);

#endif /* _FLUX_KMOD_HOOK_H */
