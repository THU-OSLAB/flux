/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_MMU_CONTEXT_H
#define _ASM_FLUX_MMU_CONTEXT_H

#include <asm/mmu.h>

struct mm_struct;
struct task_struct;

extern int init_new_context(struct task_struct *tsk, struct mm_struct *mm);
#define init_new_context init_new_context

extern void destroy_context(struct mm_struct *mm);
#define destroy_context destroy_context

#include <asm/mm_hooks.h>

static inline void switch_mm(struct mm_struct *prev,
			     struct mm_struct *next,
			     struct task_struct *tsk)
{
}

void activate_mm(struct mm_struct *prev, struct mm_struct *next);
#define activate_mm activate_mm

#include <asm-generic/mmu_context.h>

#endif /* _ASM_FLUX_MMU_CONTEXT_H */
