/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_ELF_H
#define _ASM_FLUX_ELF_H

#include <linux/types.h>
#include <asm/ptrace.h>

/* The ELF loader checks ET_EXEC/ET_DYN; the module loader checks ET_REL. */
#define elf_check_arch(x) ((x)->e_machine == EM_X86_64)
#define ELF_CLASS ELFCLASS64
#define ELF_DATA ELFDATA2LSB
#define ELF_ARCH EM_X86_64
#define ELF_EXEC_PAGESIZE 4096
#define ELF_ET_DYN_BASE 0x280000000000UL
#define ELF_PLATFORM "x86_64"
#define ELF_HWCAP flux_elf_hwcap
#define ELF_HWCAP2 flux_elf_hwcap2
#define STACK_RND_MASK 0x3fffff
#define ARCH_HAS_SETUP_ADDITIONAL_PAGES 1
#define ARCH_DLINFO NEW_AUX_ENT(AT_SYSINFO_EHDR, current->mm->context.vdso)
#define arch_elf_interpreter(path) \
	(flux_elf_interpreter && *flux_elf_interpreter ? \
	 flux_elf_interpreter : (path))

struct linux_binprm;
extern const char *flux_elf_interpreter;
extern unsigned long flux_elf_hwcap, flux_elf_hwcap2;
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp);

typedef unsigned long elf_gregset_t[27];
typedef struct { unsigned char fxsave[512]; } elf_fpregset_t;

static inline void flux_elf_core_copy_regs(elf_gregset_t dst,
					   struct pt_regs *r)
{
	dst[0] = r->r15; dst[1] = r->r14; dst[2] = r->r13;
	dst[3] = r->r12; dst[4] = r->bp; dst[5] = r->bx;
	dst[6] = r->r11; dst[7] = r->r10; dst[8] = r->r9;
	dst[9] = r->r8; dst[10] = r->ax; dst[11] = r->cx;
	dst[12] = r->dx; dst[13] = r->si; dst[14] = r->di;
	dst[15] = r->orig_ax; dst[16] = r->ip; dst[17] = 0x33;
	dst[18] = r->eflags; dst[19] = r->sp; dst[20] = 0x2b;
	dst[21] = 0; dst[22] = 0; dst[23] = 0;
	dst[24] = 0; dst[25] = 0; dst[26] = 0;
}
#define ELF_CORE_COPY_REGS(dst, regs) flux_elf_core_copy_regs(dst, regs);

struct task_struct;
int flux_elf_core_copy_task_regs(struct task_struct *task, elf_gregset_t *regs);
#define ELF_CORE_COPY_TASK_REGS(task, regs) flux_elf_core_copy_task_regs(task, regs)
#endif
