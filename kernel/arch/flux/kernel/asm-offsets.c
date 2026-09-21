/* Dummy asm-offsets.c file. Required by kbuild and ready to be used - hint! */

#include <linux/kbuild.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

void asm_offsets(void);

void asm_offsets(void)
{
	OFFSET(TASK_thread_sp, task_struct, thread.sp);
	OFFSET(TASK_thread_regs, task_struct, thread.regs);

	OFFSET(PCPU_current_task, tls_pcpu, current_task);
	OFFSET(PCPU_stack_top, tls_pcpu, stack_top);
	OFFSET(PCPU_uintr_stack_top, tls_pcpu, uintr_stack_top);
	OFFSET(PCPU_preempt_count, tls_pcpu, preempt_count);
	OFFSET(PCPU_cpu_number, tls_pcpu, cpu_number);
	OFFSET(PCPU_in_kernel, tls_pcpu, in_kernel);
	OFFSET(PCPU_host_fsbase, tls_pcpu, host_fsbase);

	OFFSET(PCPU_uintr_entry_busy, tls_pcpu, uintr_entry_busy);
	OFFSET(PCPU_uintr_entry, tls_pcpu, uintr_entry);
	OFFSET(UENTRY_rax, flux_uintr_entry_scratch, rax);
	OFFSET(UENTRY_r10, flux_uintr_entry_scratch, r10);
	OFFSET(UENTRY_r11, flux_uintr_entry_scratch, r11);
	OFFSET(UENTRY_hw_rsp, flux_uintr_entry_scratch, hw_rsp);
	OFFSET(UENTRY_fsbase, flux_uintr_entry_scratch, fsbase);
	OFFSET(UENTRY_rsi, flux_uintr_entry_scratch, rsi);
	OFFSET(UENTRY_pkru, flux_uintr_entry_scratch, pkru);

	OFFSET(REG_r11, pt_regs, r11);
	OFFSET(REG_r10, pt_regs, r10);
	OFFSET(REG_ax, pt_regs, ax);
	OFFSET(REG_orig_ax, pt_regs, orig_ax);
	OFFSET(REG_uirrv, pt_regs, uirrv);
	OFFSET(REG_ip, pt_regs, ip);
	OFFSET(REG_flags, pt_regs, flags);
	OFFSET(REG_rax, pt_regs, ax);
	OFFSET(REG_rsp, pt_regs, sp);
	OFFSET(REG_rcx, pt_regs, cx);
	OFFSET(REG_rdx, pt_regs, dx);
	OFFSET(REG_rip, pt_regs, ip);
}
