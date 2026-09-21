// SPDX-License-Identifier: GPL-2.0
/* Linux binfmt_elf owns the image. Flux prepares only its host projection
 * and validates executable bytes before Linux enters the application.
 */
#include <linux/binfmts.h>
#include <linux/elf.h>
#include <linux/entry-common.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/task_stack.h>
#include <linux/syscalls.h>
#include <asm/host_dev.h>
#include <asm/host_ops.h>
#include <asm/proc_maps.h>
#include <asm/syscalls.h>
#include <asm/vdso.h>
#include <asm/x86/syscall.h>
#include <asm/x86/fpu.h>

int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	struct mm_struct *mm = current->mm;
	unsigned long cursor = 0;
	int ret;

	/* setup_arg_pages has moved Linux's validated argument PTEs. Their new
	 * stack VMA needs a host reservation before create_elf_tables touches it.
	 */
	mmap_write_lock(mm);
	ret = flux_host_dev_reserve_alias_range(bprm->vma->vm_start,
					       bprm->vma->vm_end -
					       bprm->vma->vm_start, false);
	mmap_write_unlock(mm);
	if (ret)
		return ret;

	/* No application thread can execute this new mm until exec completes.
	 * Snapshot each interval before mprotect, which may split/merge VMAs.
	 */
	for (;;) {
		struct vm_area_struct *vma;
		unsigned long start, end, prot;

		mmap_read_lock(mm);
		vma = find_vma(mm, cursor);
		if (!vma) {
			mmap_read_unlock(mm);
			ret = flux_map_vdso(mm);
			return ret ? ret : flux_map_runtime(mm);
		}
		start = vma->vm_start;
		end = vma->vm_end;
		prot = vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC);
		mmap_read_unlock(mm);
		if (prot & PROT_EXEC) {
			ret = sys_flux_mprotect(start, end - start, prot);
			if (ret)
				return ret;
		}
		cursor = end;
	}
}

/* Bootstrap enters once from runtime setup, with the initial UIF still clear. */
long flux_kernel_exec(const char *path, char **argv, char **envp)
{
	long ret;

	arch_enter_from_user_mode(NULL);
	local_irq_enable();
	ret = kernel_execve(path, (const char *const *)argv,
			   (const char *const *)envp);
	if (ret)
		return ret;
	syscall_exit_to_user_mode(current_pt_regs());
	syscall_ret_to_user(current_pt_regs());
	__builtin_unreachable();
}

#ifdef CONFIG_COREDUMP
int flux_elf_core_copy_task_regs(struct task_struct *task, elf_gregset_t *regs)
{
	flux_elf_core_copy_regs(*regs, task_pt_regs(task));
	(*regs)[21] = task->thread.fsbase;
	return 1;
}

int elf_core_copy_task_fpregs(struct task_struct *task, elf_fpregset_t *fp)
{
	if (task == current && !test_thread_flag(TIF_NEED_FPU_LOAD)) {
		preempt_disable();
		save_xstate_full(task_xstate(task));
		preempt_enable();
	}
	memcpy(fp, task_xstate(task), sizeof(*fp));
	return 1;
}
#endif
