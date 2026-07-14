#ifndef _ASM_FLUX_PROCESSOR_H
#define _ASM_FLUX_PROCESSOR_H

#ifdef CONFIG_X86_64
#include <asm/x86/processor.h>
#else
#error "Only x86_64 is supported"
#endif

#ifndef CONFIG_MMU

/* We don't have strict user/kernel spaces */
#define TASK_SIZE ((unsigned long)-1)
#define TASK_UNMAPPED_BASE 0

/*
 * All 32bit addresses are effectively valid for vmalloc...
 * Sort of meaningless for non-VM targets.
 */
#define	VMALLOC_START		0
#define	VMALLOC_END		0xffffffff
#define	KMAP_START		0
#define	KMAP_END		0xffffffff

#else

/*
 * Default System Memory Map on Flux
 *
 * We assume no memory region overlaps with mapped regions of
 * the address space. Data and code sections are located out of
 * the directly mapped memory.
 *
 * PAGE_OFFSET (0x0) ----------- (Lower Memory) --------------------
 * 0x0000_0000_0000		0x1000_0000_0000 	(kernel direct mapped)
 * -----------------------------------------------------------------
 * 
 * ----------------------------- (Higher Memory) ---------------------
 * 0x1000_0000_0000		0x2000_0000_0000	(kvaddr: vmalloc/modules/pkmap..) 
 * 0x2000_0000_0000		0x4000_0000_0000	(user vaddr: STACK_TOP)
 * 0x4000_0000_0000		0x8000_0000_0000	(user vaddr: TASK_SIZE)
 */

#define TASK_SIZE 			0x800000000000
#define TASK_UNMAPPED_BASE 	0x300000000000
#define STACK_TOP 			0x400000000000
#define STACK_TOP_MAX 		STACK_TOP

#define KERNEL_UNMAPPED_BASE 	0x80000000000
#define VMALLOC_START 		0x100000000000
#define PKMAP_BASE 			(((VMALLOC_START - 2 * PAGE_SIZE) - LAST_PKMAP * PAGE_SIZE) & PMD_MASK)
#define VMALLOC_END			0x200000000000
#define MODULES_VADDR		VMALLOC_START
#define MODULES_END			VMALLOC_END
#define MODULES_LEN			(MODULES_VADDR - MODULES_END)

#endif // CONFIG_MMU

void start_thread(struct pt_regs *regs, unsigned long new_ip,
		  unsigned long new_sp);

#define KSTK_EIP(tsk) (0)
#define KSTK_ESP(tsk) (0)

#endif
