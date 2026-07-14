/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_SMP_H
#define _ASM_FLUX_SMP_H
#ifndef __ASSEMBLY__

#include <asm/current.h>
#include <asm/cpu.h>
#include <uapi/asm/host_ops.h>

extern char **early_debug_buf;
#define flux_debug_buf early_debug_buf[get_host_cpu_id()]
#define flux_debug(fmt, ...)                                          \
	do {                                                         \
		sprintf(flux_debug_buf, "[%2d] <flux kernel>: " fmt,   \
			get_host_cpu_id(), ##__VA_ARGS__);           \
		flux_ops_print(flux_debug_buf, strlen(flux_debug_buf)); \
	} while (0)
#define flux_bug(fmt, ...)                                                    \
	do {                                                                 \
		printk("[%2d] flux: " fmt, get_host_cpu_id(), ##__VA_ARGS__); \
		BUG();                                                       \
	} while (0)

#ifdef CONFIG_SMP

struct task_struct;
struct cpumask;

static inline void smp_prepare_boot_cpu(void)
{
}
extern void smp_prepare_cpus(unsigned int max_cpus);

extern int __cpu_up(unsigned int cpu, struct task_struct *idle);
static inline void smp_send_stop(void)
{
}
static inline void smp_cpus_done(unsigned int max_cpus)
{
}

extern void arch_smp_send_reschedule(int cpu);
extern void arch_send_call_function_single_ipi(int cpu);
extern void arch_send_call_function_ipi_mask(const struct cpumask *mask);

/*
 * This function is needed by all SMP systems. It must _always_ be valid
 * from the initial startup.
 */
#define raw_smp_processor_id() this_cpu_read(tls_pcpu.cpu_number)
#define __smp_processor_id() __this_cpu_read(tls_pcpu.cpu_number)

extern void smp_init_cpus(void);

void __noreturn start_secondary(int cpu);

void flux_tick_broadcast(int cpu);
void flux_shutdown(int cpu);
void flux_cpu_exit(void);
void flux_cpu_clock_init(int cpu);

extern int flux_ipi_gate_open(void);

int default_timer_init(void);

/**
 * flux_run_on_cpu - run a function on a specific physical CPU
 */
flux_thread_t flux_run_on_cpu(int cpu, void (*fn)(int), void *arg);

enum { FLUX_NR_STATS = 32 };

DECLARE_PER_CPU(uint64_t[2 * FLUX_NR_STATS], pcpu_stat);
#define pcpu_stat_set(stat, val) (this_cpu_ptr(pcpu_stat)[stat] = val)
#define pcpu_stat_inc(stat, inc) (this_cpu_ptr(pcpu_stat)[stat] += inc)
#define pcpu_stat_get(stat) (this_cpu_ptr(pcpu_stat)[stat])
#define pcpu_stat_foreach(n)                                              \
	for (int i = 0; i < n; i++) {                                     \
		flux_debug("CPU %d: %d=%llu\n", raw_smp_processor_id(), i, \
			  this_cpu_ptr(pcpu_stat)[i]);                    \
	}

#define pcpu_stat_clear(n)                              \
	do {                                            \
		for (int i = 0; i < n; i++) {           \
			this_cpu_ptr(pcpu_stat)[i] = 0; \
		}                                       \
	} while (0);

static inline u64 now_tsc(void)
{
	u32 lo, hi;
	barrier();
	asm volatile("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
	barrier();
	return ((u64)hi << 32) | lo;
}

#else

void flux_tick_broadcast(int cpu);
void flux_shutdown(int cpu);
void flux_cpu_exit(void);
void flux_cpu_clock_init(int cpu);
int default_timer_init(void);

#endif /* CONFIG_SMP */

extern void flux_may_change_sched_class(struct task_struct *p);

#ifdef CONFIG_FLUX_UINTR
extern int uintr_timer_init(void);
#endif /* CONFIG_FLUX_UINTR */

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_X86_SMP_H */
