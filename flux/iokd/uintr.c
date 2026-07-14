#define FLUX_FMT "iokd-uintr: "

#include <immintrin.h>

#include <kernel/asm/flux_ops.h>

#include "iokd.h"

void flux_iokd_notify_timer(struct flux_iokd_client *client, int cpu_idx)
{
#ifdef CONFIG_FLUX_UINTR
	if (!client || !flux_shm || cpu_idx < 0 || cpu_idx >= client->nr_cpus)
		return;

	_senduipi(FLUX_UINTR_VECTOR_TIMER * flux_shm->nr_cpus +
		  client->cpu_list[cpu_idx]);
#endif
}
