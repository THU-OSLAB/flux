#ifndef _FLUX_HOST_UINTR_H
#define _FLUX_HOST_UINTR_H

#include <pthread.h>

struct flux_uipi_pcpu;

extern int flux_uintr_init(void);
extern int flux_uintr_fini(void);

/**
 * flux_uintr_init_percpu - initialize uintr for a specific CPU.
 *
 * This function must be called from all available CPUs.
 *
 * @cpu: CPU number
 */
extern int flux_uintr_init_percpu(int cpu);

/**
 * flux_uintr_register_ipi - register UINTR IPI for a specific CPU and vector.
 *
 * This function must be called from all available CPUs.
 *
 * @cpu: CPU number
 * @vector: IPI vector number
 */
extern int flux_uintr_register_ipi(int cpu, int vector);

extern struct flux_uipi_pcpu *flux_uipi;

#endif /* _FLUX_HOST_UINTR_H */
