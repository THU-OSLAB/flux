#ifndef _ASM_FLUX_IRQ_H
#define _ASM_FLUX_IRQ_H

#define IRQ_STATUS_BITS (sizeof(long) * 8)
#define NR_IRQS ((int)(IRQ_STATUS_BITS * IRQ_STATUS_BITS))

#define FLUX_IRQ_BASE (NR_IRQS - 128)
#define FLUX_IRQ_IPI FLUX_IRQ_BASE

#endif
