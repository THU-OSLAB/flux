/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_PROC_MAPS_H
#define _ASM_FLUX_PROC_MAPS_H

#include <linux/types.h>

struct mm_struct;

int flux_map_runtime(struct mm_struct *mm);
long arch_proc_maps_ioctl(struct mm_struct *mm, unsigned int cmd,
			  unsigned long arg);

#endif /* _ASM_FLUX_PROC_MAPS_H */
