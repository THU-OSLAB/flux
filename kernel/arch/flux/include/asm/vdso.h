/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_VDSO_H
#define _ASM_FLUX_VDSO_H

#define FLUX_VVAR_PAGES 2

#ifndef __ASSEMBLY__
struct mm_struct;
struct vdso_data;

extern struct vdso_data *flux_vdso_data;
void flux_vdso_init(void);
int flux_map_vdso(struct mm_struct *mm);
#endif

#endif
