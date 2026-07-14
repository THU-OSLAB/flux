/* Dummy asm-offsets.c file. Required by kbuild and ready to be used - hint! */

#include <linux/kbuild.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

void asm_offsets(void);

void asm_offsets(void)
{
	OFFSET(TASK_thread_sp, task_struct, thread.sp);

	OFFSET(PCPU_current_task, tls_pcpu, current_task);
	OFFSET(PCPU_stack_top, tls_pcpu, stack_top);
	OFFSET(PCPU_uintr_stack_top, tls_pcpu, uintr_stack_top);
	OFFSET(PCPU_preempt_count, tls_pcpu, preempt_count);
	OFFSET(PCPU_in_kernel, tls_pcpu, in_kernel);

	OFFSET(REG_rsp, pt_regs, sp);
	OFFSET(REG_flags, pt_regs, flags);
#ifdef CONFIG_FLUX_MPK
	OFFSET(REG_rax, pt_regs, ax);
	OFFSET(REG_rcx, pt_regs, cx);
	OFFSET(REG_rdx, pt_regs, dx);
	OFFSET(REG_rip, pt_regs, ip);
#endif
}
