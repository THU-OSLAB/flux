/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux TLB hooks synchronously revoke host projections before source-page
 * reuse. Faults rebuild aliases from current Linux PTEs; there is no second
 * page-table scan or acknowledgement protocol.
 */
#ifndef _ASM_FLUX_TLBFLUSH_H
#define _ASM_FLUX_TLBFLUSH_H

#include <linux/mm.h>

void flux_flush_tlb_gather(struct mm_struct *mm, unsigned long start,
			   unsigned long end);

extern void flush_tlb_all(void);
extern void flush_tlb_mm(struct mm_struct *mm);
extern void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
			    unsigned long end);
extern void flush_tlb_page(struct vm_area_struct *vma, unsigned long address);
extern void flush_tlb_kernel_vm(void);
extern void flush_tlb_kernel_range(unsigned long start, unsigned long end);
extern void __flush_tlb_one(unsigned long addr);

#endif /* _ASM_FLUX_TLBFLUSH_H */
