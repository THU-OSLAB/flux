#include <linux/memory.h>
#include <linux/kernel.h>
#include <linux/binfmts.h>
#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/personality.h>
#include <linux/fs.h>
#include <linux/reboot.h>
#include <linux/start_kernel.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <uapi/linux/sched/types.h>
#include <linux/tick.h>
#include <linux/memblock.h>
#include <asm/host_ops.h>
#include <asm/irq.h>
#include <asm/unistd.h>
#include <asm/setup.h>
#include <asm/syscalls.h>
#include <asm/smp.h>
#include <asm/kasan.h>
#include <asm/spdk.h>
#include <asm/flux_ops.h>
#include <asm/host_dev.h>
#include <asm/signal.h>

#include <asm/x86/tsc.h>

#ifdef CONFIG_FLUX_UINTR
#include <asm/x86/uintr.h>

extern struct flux_uipi_pcpu flux_uipi[NR_CPUS];
#endif

int is_running;

struct flux_host_operations *flux_ops __read_mostly;
unsigned long *flux_host_fsbases __read_mostly;
void *flux_vvar_data __read_mostly;
struct flux_spdk *flux_spdk __read_mostly;
struct flux_spdk_operations *flux_spdk_ops __read_mostly;
static void (*flux_main_entry)(void *);

static char cmd_line[COMMAND_LINE_SIZE];
static char *cmd_line_ptr __initdata = boot_command_line;
static int cmd_line_len __initdata = COMMAND_LINE_SIZE;

DEFINE_EARLY_PER_CPU(bool, init_done, false);
DEFINE_EARLY_PER_CPU(bool, boot_flag, false);

char **early_debug_buf;

long flux_panic_blink(int state)
{
	flux_ops_panic();
	return 0;
}

static unsigned long mem_size = MIN_MEMORY_BLOCK_SIZE;
static unsigned long dma_size = 0;

static int __init setup_mem_size(char *str)
{
	mem_size = memparse(str, NULL);
	return 0;
}
early_param("mem", setup_mem_size);

static int __init setup_dma_size(char *str)
{
#ifdef CONFIG_ZONE_DMA
	dma_size = memparse(str, NULL);
	if (dma_size < MIN_MEMORY_BLOCK_SIZE)
		dma_size = MIN_MEMORY_BLOCK_SIZE;
#endif
	return 0;
}
early_param("dma_size", setup_dma_size);

void __init setup_arch(char **cl)
{
	int i;

	*cl = cmd_line;

	panic_blink = flux_panic_blink;

	fill_cpuinfo(&boot_cpu_data);

	parse_early_param();

	reset_cpu_possible_mask();
	for (i = 0; i < NR_CPUS; i++)
		set_cpu_possible(i, true);

	bootmem_init(mem_size, dma_size);
	misc_mem_init();
}

#ifdef CONFIG_SMP
static DEFINE_PER_CPU(struct cpu, cpu_devices);

static int __init topology_init(void)
{
	int i, ret;

	for_each_possible_cpu(i) {
		struct cpu *cpu = &per_cpu(cpu_devices, i);

		cpu->hotpluggable = false;
		ret = register_cpu(cpu, i);
		if (unlikely(ret))
			pr_warn("%s: register_cpu %d failed (%d)\n", __func__,
				i, ret);
	}

	return 0;
}
subsys_initcall(topology_init);
#endif

static __init void cmd_line_append_va(const char *fmt, va_list ap)
{
	int ret;

	ret = vsnprintf(cmd_line_ptr, cmd_line_len, fmt, ap);

	if (ret > 0) {
		cmd_line_ptr += ret;
		cmd_line_len -= ret;
	}
}

static inline __init void cmd_line_append(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	cmd_line_append_va(fmt, ap);
	va_end(ap);
}

static void flux_uintr_init_ipi(int cpu)
{
	while (!flux_ops)
		cpu_relax();
#ifdef CONFIG_FLUX_UINTR
#ifdef CONFIG_FLUX_IPI_GATE
	flux_ops->uintr_register_ipi(cpu, 0);
#else
	flux_ops->uintr_register_ipi(cpu, FLUX_IPI_EXIT);
	flux_ops->uintr_register_ipi(cpu, FLUX_IPI_RESCHED);
	flux_ops->uintr_register_ipi(cpu, FLUX_IPI_CALLFUNC);
	flux_ops->uintr_register_ipi(cpu, FLUX_IPI_TICKBC);
	flux_ops->uintr_register_ipi(cpu, FLUX_IPI_SHUTDOWN);
#endif
#endif
}

void __init __noreturn flux_start_kernel(struct flux_host_info *host,
					 const char *fmt, ...)
{
	int i;
	va_list ap;

	/* init host ops */
	flux_ops = host->ops;
	flux_host_fsbases = host->fsbases;
	flux_vvar_data = host->vvar;
	if (!flux_vvar_data)
		panic("missing Flux vvar writer mapping");
#ifdef CONFIG_FLUX_SPDK
	flux_spdk = host->spdk;
	flux_spdk_ops = &flux_spdk->ops;
#endif
	flux_main_entry = host->main;

	/* init early debug buffer */
	early_debug_buf =
		(char **)flux_ops->mem_alloc(host->max_cpus * sizeof(char *));
	for (i = 0; i < host->max_cpus; i++)
		early_debug_buf[i] = (char *)flux_ops->mem_alloc(4096);

	flux_host_dev_init();

#ifdef CONFIG_FLUX_MPK
	if (flux_host_dev_enable_mpk())
		panic("failed to enable host MPK policy");
#endif

	/* init command line */
	cmd_line_append("%s ", CONFIG_BUILTIN_CMDLINE);
	va_start(ap, fmt);
	cmd_line_append_va(fmt, ap);
	va_end(ap);
	memcpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);

#ifdef CONFIG_SMP
	/* init stat */
	memset(pcpu_stat, 0, sizeof(pcpu_stat));
#else
	tls_pcpu.cpu_number = 0;
	tls_pcpu.host_tid = flux_ops_gettid_raw();
#endif

	/* init uintr ipi */
	flux_uintr_init_ipi(0);

	early_per_cpu(init_done, 0) = true;
	for (i = 1; i < NR_CPUS; i++) {
		while (!early_per_cpu(init_done, i))
			cpu_relax();
	}

	/* copy uipi info after all init done */
#ifdef CONFIG_FLUX_UINTR
	memcpy(&flux_uipi, host->uipi, sizeof(struct flux_uipi_pcpu) * NR_CPUS);
#endif

	start_kernel();
}

#ifdef CONFIG_SMP
void __init __noreturn flux_start_kernel_secondary(int cpu)
{
	flux_uintr_init_ipi(cpu);

	while (!early_per_cpu(init_done, 0))
		cpu_relax();

	early_per_cpu(init_done, cpu) = true;

	while (!early_per_cpu(boot_flag, cpu))
		cpu_relax();

	start_secondary(cpu);
}
#endif

void flux_may_change_sched_class(struct task_struct *p)
{
#ifdef CONFIG_FLUX_SCHED_FIFO
	{
		struct sched_param param = { .sched_priority = 1 };
		sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
		pr_info("setup: fifo scheduler\n");
	}
#elif defined(CONFIG_FLUX_SCHED_RR)
	{
		struct sched_param param = { .sched_priority = 1 };
		sched_setscheduler_nocheck(p, SCHED_RR, &param);
		pr_info("setup: round-robin scheduler\n");
	}
#else
	/* no-op */
	pr_info("setup: normal scheduler\n");
#endif
}

void *flux_kmalloc(unsigned long size)
{
	return kmalloc(size, irqs_disabled() ? GFP_ATOMIC : GFP_KERNEL);
}

void flux_kfree(const void *addr)
{
	kfree(addr);
}

int flux_is_kernel_memory(const void *ptr)
{
	unsigned long addr = (unsigned long)ptr;

	return addr >= memory_start && addr < memory_end;
}

static int flux_run_init(struct linux_binprm *bprm);

static struct linux_binfmt flux_run_init_binfmt = {
	.module = THIS_MODULE,
	.load_binary = flux_run_init,
};

struct mm_struct *main_mm = NULL;

static int main_entry_wrapper(void *unused)
{
	/* kernel_thread() marks the child even when its parent was normalized. */
	current->flags &= ~PF_KTHREAD;

	/* now we first enter user mode */
	this_cpu_write(tls_pcpu.in_kernel, false);

	pr_info("starting main entry\n");

	flux_main_entry(NULL);

	return 0;
}

int init(void *unused)
{
	struct thread_info *ti = current_thread_info();
	struct rlimit rlim;
	struct task_struct *tsk = current;
	pid_t pid;
	int ret = 0, stat;

	if (!flux_main_entry)
		return -EINVAL;

	main_mm = mm_alloc();
	if (!main_mm)
		return -ENOMEM;

	arch_pick_mmap_layout(main_mm, NULL);

	tsk->min_flt = tsk->maj_flt = 0;
	tsk->nvcsw = tsk->nivcsw = 0;

	mmget(main_mm);
	tsk->mm = main_mm;
	tsk->active_mm = main_mm;
	sched_mm_cid_fork(tsk);

	/* set flag for interrupt handler */
	set_ti_thread_flag(ti, TIF_USER);

#define FLUX_KTHREAD_MAX_FDS 1048575
	/* set max fds (kthread cannot inherit rlimit from kthreadd) */
	rlim.rlim_cur = FLUX_KTHREAD_MAX_FDS;
	rlim.rlim_max = FLUX_KTHREAD_MAX_FDS;
	sys_setrlimit(RLIMIT_NOFILE, &rlim);

	/* we should set following threads back to normal affinity */
	set_cpus_allowed_ptr(current, cpu_possible_mask);

	/* may change default scheduler and all childs inherit scheduler class */
	flux_may_change_sched_class(current);

	/* I'm not a kthread! */
	current->flags &= ~PF_KTHREAD;

	flux_signal_register_init_task(current);

	/* init execd before main entry */
	flux_exec_init();

	pid = kernel_thread(main_entry_wrapper, NULL, "main",
			    CLONE_FS | CLONE_FILES | SIGCHLD);
	if (pid < 0) {
		pr_err("failed to create main: %d\n", pid);
		ret = pid;
		goto out;
	}

	/* wait task exit */
	ret = kernel_wait(pid, &stat);
	if (ret < 0) {
		/*
		 * A terminating signal aimed at init can interrupt the blocking
		 * wait on the main task. That returns -ERESTARTSYS here and the
		 * signal will still determine init's final exit status, so avoid
		 * logging it as an unexpected wait failure.
		 */
		if (ret != -ERESTARTSYS)
			pr_err("wait main failed: %d\n", ret);
		goto out;
	}

out:
	flux_signal_unregister_init_task(current);

	return ret;
}

static int flux_run_init(struct linux_binprm *bprm)
{
	int ret, stat;
	pid_t pid;

	if (strcmp("/init", bprm->filename) != 0)
		return -EINVAL;

	ret = begin_new_exec(bprm);
	if (ret)
		return ret;

	set_personality(PER_LINUX);
	setup_new_exec(bprm);
	set_binfmt(&flux_run_init_binfmt);

	/* run init as a daemon */
	set_cpus_allowed_ptr(current, cpumask_of(0));
	snprintf(current->comm, sizeof(current->comm), "flux_idle/0");
	flux_cpu_clock_init(0);

	pid = kernel_thread(init, NULL, "init",
			    CLONE_FS | CLONE_FILES | SIGCHLD);
	if (pid < 0) {
		pr_err("failed to create init: %d\n", pid);
		machine_halt();
	}

	/* wait task exit */
	ret = kernel_wait(pid, &stat);
	if (ret < 0) {
		pr_err("wait init failed: %d\n", ret);
		machine_halt();
	}

	pr_info("init exited with %d\n", stat);

	machine_halt();

	/* init should never return */
	return 0;
}

/* skip mounting the "real" rootfs. ramfs is good enough. */
static int __init fs_setup(void)
{
	int fd;

	// Pad '/init' to make sure it's 8 bytes, otherwise KASan would
	// emit an error. The kernel's strncpy implementation attempts to read
	// 8 bytes at once and, thus, triggers KASan violation for the 6-byte
	// string.
	fd = sys_open("/init\0\0", O_CREAT, 0700);
	WARN_ON(fd < 0);
	sys_close(fd);

	register_binfmt(&flux_run_init_binfmt);

	return 0;
}
late_initcall(fs_setup);
