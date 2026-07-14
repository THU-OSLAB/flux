#ifndef _ASM_UAPI_FLUX_HOST_OPS_H
#define _ASM_UAPI_FLUX_HOST_OPS_H

#ifdef __KERNEL__
#include <uapi/asm/mpk.h>
#else
#include <kernel/asm/mpk.h>
#endif

#define FLUX_MEMORY_ADDR 0x700000000000

/*
 * page_alloc() uses the low bits to preserve the existing NUMA node encoding
 * and reserves upper bits for allocation-type flags.
 */
#define FLUX_PAGE_ALLOC_NODE_MASK 0x0000ffffU
#define FLUX_PAGE_ALLOC_DMA       0x00010000U

#define FLUX_PAGE_ALLOC_MAKE_FLAGS(node, flags) \
	(((node) & FLUX_PAGE_ALLOC_NODE_MASK) | (flags))
#define FLUX_PAGE_ALLOC_NODE(flags) ((flags) & FLUX_PAGE_ALLOC_NODE_MASK)
#define FLUX_PAGE_ALLOC_IS_DMA(flags) ((flags) & FLUX_PAGE_ALLOC_DMA)

/* Defined in {posix,nt}-host.c */
struct flux_mutex;
struct flux_sem;
typedef unsigned long flux_thread_t;
struct flux_timer_args {
	void (*fn)(int);
	int cpu;
	int oneshot;
};
struct flux_spdk;

enum flux_prot {
	FLUX_PROT_NONE = 0,
	FLUX_PROT_READ = 1,
	FLUX_PROT_WRITE = 2,
	FLUX_PROT_EXEC = 4,
};

typedef void (*flux_post_exec_fn_t)(void *entry, unsigned long stack);

/**
 * flux_host_operations - host operations used by the Linux kernel
 *
 * These operations must be provided by a host library or by the application
 * itself.
 *
 * @print - optional operation that receives console messages
 *
 * @panic - called during a kernel panic
 *
 * @sem_alloc - allocate a host semaphore an initialize it to count
 * @sem_free - free a host semaphore
 * @sem_up - perform an up operation on the semaphore
 * @sem_down - perform a down operation on the semaphore
 *
 * @mutex_alloc - allocate and initialize a host mutex; the recursive parameter
 * determines if the mutex is recursive or not
 * @mutex_free - free a host mutex
 * @mutex_lock - acquire the mutex
 * @mutex_unlock - release the mutex
 *
 * @thread_create - create a new thread and run f(arg) in its context; returns a
 * thread handle or 0 if the thread could not be created
 * @thread_detach - on POSIX systems, free up resources held by
 * pthreads. Noop on Win32.
 * @thread_exit - terminates the current thread
 * @thread_join - wait for the given thread to terminate. Returns 0
 * for success, -1 otherwise
 * @thread_stack - get the thread stack base and size of the current thread
 * @thread_bind - bind the current thread to the given CPU (0 <= cpu < ncpus)
 *
 * @mem_alloc - allocate memory
 * @mem_free - free memory
 * @page_alloc - allocate page aligned memory
 * @page_free - free memory allocated by page_alloc
 *
 * @timer_alloc - allocate a host timer that runs fn() when the timer
 * fires.
 * @timer_free - disarms and free the timer
 * @timer_set_oneshot - arm the timer to fire once, after delta ns.
 *
 * @ioremap - searches for an I/O memory region identified by addr and size and
 * returns a pointer to the start of the address range that can be used by
 * iomem_access
 * @iomem_acess - reads or writes to and I/O memory region; addr must be in the
 * range returned by ioremap
 *
 * @gettid - returns the host thread id of the caller, which need not
 * be the same as the handle returned by thread_create
 * @getcpu - returns the physical CPU number of the caller
 *
 * @uintr_register_ipi - register a UINTR handler for the given CPU and vector
 *
 * @load_elf - load an ELF binary from the host filesystem and prepare it for
 * execution in the kernel; the post_exec callback will be called with the entry
 * point and initial stack pointer of the loaded binary, and can perform
 * additional initialization before the kernel starts executing the binary.
 */
struct flux_host_operations {
	void (*print)(const char *str, int len);
	void (*panic)(void);
	void (*debug)(char *fmt, ...);

	struct flux_sem *(*sem_alloc)(int count);
	void (*sem_free)(struct flux_sem *sem);
	void (*sem_up)(struct flux_sem *sem);
	void (*sem_down)(struct flux_sem *sem);

	struct flux_mutex *(*mutex_alloc)(int recursive);
	void (*mutex_free)(struct flux_mutex *mutex);
	void (*mutex_lock)(struct flux_mutex *mutex);
	void (*mutex_unlock)(struct flux_mutex *mutex);

	flux_thread_t (*thread_create)(void (*f)(void *), void *arg,
				       char *name);
	void (*thread_detach)(void);
	void (*thread_exit)(void);
	void (*thread_longjmp)(int cpu);
	int (*thread_join)(flux_thread_t tid);
	flux_thread_t (*thread_self)(void);
	int (*thread_equal)(flux_thread_t a, flux_thread_t b);
	void *(*thread_stack)(unsigned long *size);
	int (*thread_bind)(int cpu);

	void *(*mem_alloc)(unsigned long);
	void (*mem_free)(void *);
	void *(*page_alloc)(void *hint, unsigned long size, unsigned long align,
			    int flags);
	void (*page_free)(void *addr, unsigned long size);
	int (*page_handle)(void *addr);

	void *(*va_to_pa)(void *addr);

	unsigned long long (*time)(void);

	void *(*timer_alloc)(struct flux_timer_args *args);
	int (*timer_set_oneshot)(void *timer, unsigned long delta);
	void (*timer_free)(void *timer);

	void *(*ioremap)(long addr, int size);
	int (*iomem_access)(const volatile void *addr, void *val, int size,
			    int write);

	long (*gettid)(void);
	int (*getcpu)(void);

	int (*uintr_register_ipi)(int cpu, int vector);

	int (*load_elf)(const char *path, char **argv, char **envp,
			flux_post_exec_fn_t fn);
};

struct flux_host_info {
	struct flux_host_operations *ops;
#ifdef CONFIG_FLUX_SPDK
	struct flux_spdk *spdk;
#endif
	struct flux_uipi_pcpu *uipi;
	void (*main)(void *);
	unsigned long *fsbases;
	void *vvar;
	int max_cpus;
};

struct flux_rodata {
	void *syscall_fast;
	void *syscall;
} __attribute__((__aligned__(4096)));

#define FLUX_RODATA_ADDR 0x100000
#define FLUX_RODATA_HEADER_SIZE 4096

extern int flux_host_dev_fd;

#endif
