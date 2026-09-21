#define _GNU_SOURCE
#include <numa.h>
#include <sched.h>

#include <flux.h>

static void flux_cpu_list_reset(void)
{
	free(flux_env.cpu_list);
	flux_env.cpu_list = NULL;
	flux_env.nr_cpus = 0;
}

static int flux_init_ctrl_cpus(void)
{
	cpu_set_t cpuset;
	int max_cpus;
	int i;
	int cpu;

	flux_env.ctrl_cpu = -1;
#ifdef CONFIG_FLUX_FNET
	if (flux_env.fnet_enabled)
		FLUX_LOG(FLUX_LOG_INFO,
			 "external flux_iokd owns timer and fnet dataplane cpu\n");
	else
#endif
		FLUX_LOG(FLUX_LOG_INFO,
			 "external flux_iokd owns timer delivery cpu\n");
	return 0;

	max_cpus = numa_num_possible_cpus();
	if (sched_getaffinity(0, sizeof(cpuset), &cpuset) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "sched_getaffinity failed\n");
		return -FLUX_EINVAL;
	}

	/* don't use CPU0! */
	if (CPU_SETSIZE > 0)
		CPU_CLR(0, &cpuset);
	for (i = 0; i < flux_env.nr_cpus; i++)
		if (flux_env.cpu_list[i] >= 0 && flux_env.cpu_list[i] < CPU_SETSIZE)
			CPU_CLR(flux_env.cpu_list[i], &cpuset);

	flux_env.ctrl_cpu = -1;
	for (cpu = 0; cpu < max_cpus && cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &cpuset))
			continue;
		flux_env.ctrl_cpu = cpu;
		break;
	}
	if (flux_env.ctrl_cpu < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "no available cpu for local control path\n");
		return -FLUX_EINVAL;
	}

	FLUX_LOG(FLUX_LOG_INFO, "local control cpu: %d\n", flux_env.ctrl_cpu);
	return 0;
}

int flux_env_set_cpus(const int *cpu_list, int nr_cpus)
{
	int max_cpus;
	int *copy;
	int i;

	max_cpus = numa_num_possible_cpus();
	if (nr_cpus <= 0 || nr_cpus > CONFIG_FLUX_MAX_CPUS)
		return -FLUX_EINVAL;

	copy = calloc((size_t)nr_cpus, sizeof(*copy));
	if (!copy)
		return -FLUX_ENOMEM;

	for (i = 0; i < nr_cpus; i++) {
		if (cpu_list[i] < 0 || cpu_list[i] >= max_cpus) {
			free(copy);
			return -FLUX_EINVAL;
		}
		copy[i] = cpu_list[i];
	}

	flux_cpu_list_reset();
	flux_env.cpu_list = copy;
	flux_env.nr_cpus = nr_cpus;
	flux_env.max_cpus = max_cpus;
	return 0;
}

static int flux_select_local_cpus(void)
{
	cpu_set_t cpuset;
	int cpu_list[CONFIG_FLUX_MAX_CPUS];
	int nr = 0;
	int max_cpus;
	int cpu;

	max_cpus = numa_num_possible_cpus();
	if (sched_getaffinity(0, sizeof(cpuset), &cpuset) < 0)
		return -FLUX_EINVAL;

	for (cpu = 0; cpu < max_cpus && cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &cpuset))
			continue;
		cpu_list[nr++] = cpu;
		if (nr == CONFIG_FLUX_MAX_CPUS)
			break;
	}

	if (nr != CONFIG_FLUX_MAX_CPUS)
		return -FLUX_EINVAL;

	return flux_env_set_cpus(cpu_list, nr);
}

int flux_init_cpus(void)
{
	int ret;

	if (!flux_env.cpu_list || flux_env.nr_cpus <= 0) {
		ret = flux_select_local_cpus();
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to auto-select local cpu list\n");
			return ret;
		}
	}

	ret = flux_init_ctrl_cpus();
	if (ret < 0)
		return ret;

	return 0;
}

int flux_bind_single_cpu(int cpu)
{
	int ret;
	cpu_set_t cpu_set;

	CPU_ZERO(&cpu_set);
	CPU_SET(cpu, &cpu_set);

	ret = sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set);
	if (ret < 0)
		return ret;

	while (sched_getcpu() != cpu) {
		FLUX_LOG(FLUX_LOG_ERR, "waiting for cpu %d\n", cpu);
		sched_yield();
	}

	return ret;
}
