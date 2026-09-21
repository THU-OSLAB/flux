/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_TLB_H
#define _ASM_FLUX_TLB_H

#include <linux/mm.h>
#include <asm/tlbflush.h>
/* The generic flush also handles fullmm forced by concurrent PTE gathers. */
#include <asm-generic/tlb.h>

#endif /* _ASM_FLUX_TLB_H */
