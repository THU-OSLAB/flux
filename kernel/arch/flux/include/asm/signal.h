/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_SIGNAL_H
#define _ASM_FLUX_SIGNAL_H

#ifdef CONFIG_X86_64

#ifndef __ASSEMBLY__
#include <linux/linkage.h>

/* Most things should be clean enough to redefine this at will, if care
   is taken to make libc match.  */

#define _NSIG 64

#define _NSIG_BPW 64

#define _NSIG_WORDS (_NSIG / _NSIG_BPW)

typedef unsigned long old_sigset_t; /* at least 32 bits */

typedef struct {
	unsigned long sig[_NSIG_WORDS];
} sigset_t;

#endif /* __ASSEMBLY__ */
#include <uapi/asm/signal.h>
#ifndef __ASSEMBLY__

#define __ARCH_HAS_SA_RESTORER

#include <asm/x86/asm.h>
#include <uapi/asm/sigcontext.h>

#undef __HAVE_ARCH_SIG_BITOPS
#endif /* __ASSEMBLY__ */

#else
#error "Only x86_64 is supported in Flux"
#endif

#ifdef CONFIG_FLUX_UINTR

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>

struct task_struct;
struct pid;

struct flux_sig_entry {
	int sig_nr;
	int ctrl_op;
	unsigned int ctrl_arg;
	int sig_code;
	unsigned long sig_addr;
	struct pid *sig_pid;
	struct list_head sig_link;
};

struct flux_sig_list {
	spinlock_t lock;
	struct list_head head;
};

struct flux_sig_entry *flux_sig_take_entry(int cpu);
void flux_signal_register_init_task(struct task_struct *task);
void flux_signal_unregister_init_task(struct task_struct *task);

#ifdef CONFIG_FLUX_RUNC
int flux_exec_init(void);
void flux_exec_wake(void);
#else
static inline int flux_exec_init(void)
{
	return 0;
}

static inline void flux_exec_wake(void)
{
}
#endif

#else

struct task_struct;

static inline void flux_signal_register_init_task(struct task_struct *task)
{
}

static inline void flux_signal_unregister_init_task(struct task_struct *task)
{
}

static inline int flux_exec_init(void)
{
	return 0;
}

static inline void flux_exec_wake(void)
{
}

#endif /* CONFIG_FLUX_UINTR */

#endif /* _ASM_FLUX_SIGNAL_H */
