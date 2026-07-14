/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_SPARSEMEM_H
#define _ASM_FLUX_SPARSEMEM_H

#include <linux/types.h>

#ifdef CONFIG_SPARSEMEM
/*
 * generic non-linear memory support:
 *
 * 1) we will not split memory into more chunks than will fit into the flags
 *    field of the struct page
 *
 * SECTION_SIZE_BITS		2^n: size of each section
 * MAX_PHYSMEM_BITS		2^n: max size of physical address space
 *
 */

#define SECTION_SIZE_BITS 28 /* 256MB */
#define MAX_PHYSMEM_BITS 57

#endif /* CONFIG_SPARSEMEM */

#endif /* _ASM_FLUX_SPARSEMEM_H */
