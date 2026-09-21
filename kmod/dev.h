#ifndef _FLUX_KMOD_DEV_H
#define _FLUX_KMOD_DEV_H

#include <flux/host_abi.h>

#include "uintr.h"

struct flux_mm_ctx;
struct flux_proc;

struct flux_percpu {
	struct uintr_percpu uintr;
	struct flux_mm_ctx *active_mm_ctx;
	struct flux_proc *active_proc;
	struct task_struct *active_mm_task;
};
DECLARE_PER_CPU(struct flux_percpu, flux_percpu);

extern __read_mostly struct flux_shm *flux_shm;

#endif /* _FLUX_KMOD_DEV_H */
