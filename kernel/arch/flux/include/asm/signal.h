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


#include <linux/types.h>
#include <linux/llist.h>

struct pt_regs;
struct flux_deferred_fault {
	unsigned long address;
	unsigned long error;
	int code;
	int signal;
};

void flux_take_deferred_fault(struct flux_deferred_fault *fault);
void flux_complete_user_fault(struct pt_regs *regs,
			      const struct flux_deferred_fault *fault);
void flux_complete_kernel_fault(struct pt_regs *regs,
				const struct flux_deferred_fault *fault);

struct task_struct;
struct pid;

struct flux_sig_entry {
	int sig_nr;
	int ctrl_op;
	unsigned int ctrl_arg;
	int sig_code;
	unsigned long sig_addr;
	bool sig_has_info;
	int sig_info_errno;
	int sig_info_pid;
	unsigned int sig_info_uid;
	struct pid *sig_pid;
	struct llist_node sig_node;
};

struct flux_sig_list {
	struct llist_head pending;
};

struct llist_node *flux_sig_take_batch(int cpu);
void flux_signal_register_init_task(struct task_struct *task);
void flux_signal_unregister_init_task(struct task_struct *task);

#ifdef CONFIG_FLUX_RUNC
int flux_exec_init(void);
void flux_exec_wake(void);
void flux_exec_record_init_status(int status);
#else
static inline int flux_exec_init(void)
{
	return 0;
}

static inline void flux_exec_wake(void)
{
}

static inline void flux_exec_record_init_status(int status)
{
}
#endif


#endif /* _ASM_FLUX_SIGNAL_H */
