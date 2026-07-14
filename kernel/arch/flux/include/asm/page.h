/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_PAGE_H
#define _ASM_FLUX_PAGE_H

#define ARCH_PFN_OFFSET (memory_start >> PAGE_SHIFT)
#include <asm-generic/page.h>

#ifndef __ASSEMBLY__
void bootmem_init(unsigned long mem_size, unsigned long dma_size);
void misc_mem_init(void);
#endif

#undef PAGE_OFFSET
#define PAGE_OFFSET memory_start

#endif /* _ASM_FLUX_PAGE_H */
