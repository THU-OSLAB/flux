/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_MMU_H
#define _ASM_FLUX_MMU_H

#ifndef __ASSEMBLY__
typedef struct {
	unsigned long end_brk;
	int proc_key;
} mm_context_t;

#define INIT_MM_CONTEXT(name) .context.proc_key = 0,

#endif

#endif /* _ASM_FLUX_MMU_H */
