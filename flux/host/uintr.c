#define FLUX_FMT "uintr: "

#define _GNU_SOURCE
#include <immintrin.h>
#include <numa.h>
#include <sched.h>
#include <stdlib.h>

#include <flux.h>

#include "kmod.h"
#include "uintr.h"

struct flux_uipi_pcpu *flux_uipi;

static int flux_uintr_register_vec(int cpu, int uvec)
{
	int ret, i;
	int nr_cpus = flux_env.nr_cpus;
	struct flux_uipi_vec_info *local;

	if (nr_cpus <= 0)
		return -FLUX_EINVAL;

	if (cpu < 0 || cpu >= nr_cpus || uvec < 0 || uvec >= FLUX_UINTR_VEC_NR)
		return -FLUX_EINVAL;

	local = &flux_uipi[cpu].vecs[uvec];

	local->uitt = calloc((size_t)nr_cpus, sizeof(int));
	if (!local->uitt) {
		ret = -FLUX_ENOMEM;
		goto out;
	}

	for (i = 0; i < nr_cpus; i++)
		local->uitt[i] =
			uvec * flux_shm->nr_cpus + flux_env.cpu_list[i];

	return 0;
out:
	return ret;
}

static void flux_uintr_free_vecs(int nr_cpus)
{
	int cpu, uvec;

	if (!flux_uipi)
		return;

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		for (uvec = 0; uvec < FLUX_UINTR_VEC_NR; uvec++) {
			free(flux_uipi[cpu].vecs[uvec].uitt);
			flux_uipi[cpu].vecs[uvec].uitt = NULL;
		}
	}
}

int flux_uintr_register_ipi(int cpu, int vector)
{
	return flux_uintr_register_vec(cpu, FLUX_UINTR_VEC_IPI_BASE + vector);
}

int flux_uintr_init(void)
{
	int err, cpu;
	int nr_cpus = flux_env.nr_cpus;

	if (nr_cpus <= 0)
		return -FLUX_EINVAL;

	flux_uipi = calloc((size_t)nr_cpus, sizeof(struct flux_uipi_pcpu));
	if (!flux_uipi) {
		err = -FLUX_ENOMEM;
		goto out;
	}

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		err = flux_uintr_register_vec(cpu, FLUX_UINTR_VECTOR_TIMER);
		if (err < 0)
			goto out_free_uipi;

		err = flux_uintr_register_vec(cpu, FLUX_UINTR_VECTOR_SIGNAL);
		if (err < 0)
			goto out_free_uipi;
	}

	FLUX_LOG(FLUX_LOG_INFO,
		 "external flux_iokd owns UINTR timer delivery\n");

	return 0;
out_free_uipi:
	flux_uintr_free_vecs(nr_cpus);
	free(flux_uipi);
	flux_uipi = NULL;
out:
	return err;
}

int flux_uintr_fini(void)
{
	int nr_cpus = flux_env.nr_cpus;

	flux_uintr_free_vecs(nr_cpus);
	free(flux_uipi);
	flux_uipi = NULL;

	return 0;
}
