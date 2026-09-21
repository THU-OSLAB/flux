#ifndef _ASM_FLUX_THREAD_INFO_H
#define _ASM_FLUX_THREAD_INFO_H

#include <asm/page.h>

#ifdef CONFIG_KASAN
#define KASAN_STACK_ORDER 1
#else
#define KASAN_STACK_ORDER 0
#endif

/* Thread information allocation.  */
#define THREAD_SIZE_ORDER (4 + KASAN_STACK_ORDER)
#define THREAD_SIZE (PAGE_SIZE << THREAD_SIZE_ORDER)

#define IRQ_STACK_ORDER (2 + KASAN_STACK_ORDER)
#define IRQ_STACK_SIZE (PAGE_SIZE << IRQ_STACK_ORDER)

#ifndef __ASSEMBLY__

#include <asm/types.h>

struct task_struct;

struct thread_info {
	unsigned long flags; /* low level flags */
	unsigned long syscall_work; /* SYSCALL_WORK_ flags */
#ifdef CONFIG_SMP
	u32 cpu;
#endif
};

#define INIT_THREAD_INFO(tsk)                           \
	{                                               \
		.flags = 0,                             \
	}

#else /* !__ASSEMBLY__ */

#include <asm/asm-offsets.h>

#endif

#define TIF_SYSCALL_TRACE	0	/* syscall trace active */
#define TIF_NOTIFY_RESUME	1	/* callback before returning to user */
#define TIF_SIGPENDING		2	/* signal pending */
#define TIF_NEED_RESCHED	3	/* rescheduling necessary */
#define TIF_USER 			10  /* kthread created by user */
#define TIF_NEED_FPU_LOAD	11	/* load FPU */
#define TIF_POLLING_NRFLAG	16	/* idle polls TIF_NEED_RESCHED */
#define TIF_NOTIFY_SIGNAL	17	/* signal notifications exist */
#define TIF_MEMDIE			20	/* is terminating due to OOM killer */
#define TIF_NOTSC			21	/* RDTSC should fault in userspace */

#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_POLLING_NRFLAG	(1 << TIF_POLLING_NRFLAG)
#define _TIF_NOTIFY_SIGNAL	(1 << TIF_NOTIFY_SIGNAL)
#define _TIF_NOTSC		(1 << TIF_NOTSC)

#ifndef __ASSEMBLY__
void disable_TSC(void);
void enable_TSC(void);
#endif

#endif
