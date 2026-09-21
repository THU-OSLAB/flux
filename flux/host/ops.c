// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _DEFAULT_SOURCE /* glibc preadv/pwritev */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <flux.h>
#include <flux/mpk.h>
#include <flux/rewrite.h>

#include "io/iok_client.h"
#include "kmod.h"
#include "uintr.h"

#ifdef _POSIX_SEMAPHORES
#include <semaphore.h>
/* TODO(pscollins): We don't support fork() for now, but maybe one day
 * we will? */
#define SHARE_SEM 0
#else
#error "POSIX semaphores not supported, please enable _POSIX_SEMAPHORES in your build system"
#endif /* _POSIX_SEMAPHORES */

static void write_stdout(const char *str, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t ret = write(STDOUT_FILENO, str + off, len - off);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			break;
		off += (size_t)ret;
	}
}

static void print(const char *str, int len)
{
	size_t start = 0;
	size_t i;

	if (!str || len <= 0)
		return;

	if (!flux_stdout_needs_crlf()) {
		write_stdout(str, (size_t)len);
		return;
	}

	for (i = 0; i < (size_t)len; i++) {
		if (str[i] != '\n' || (i && str[i - 1] == '\r'))
			continue;

		write_stdout(str + start, i - start);
		write_stdout("\r\n", 2);
		start = i + 1;
	}

	write_stdout(str + start, (size_t)len - start);
}

#define WARN_UNLESS(exp)                                           \
	do {                                                       \
		if (exp < 0)                                       \
			printf("%s: %s\n", #exp, strerror(errno)); \
	} while (0)

static int warn_pthread(int ret, char *str_exp)
{
	if (ret > 0)
		FLUX_LOG(FLUX_LOG_WARN, "%s: %s\n", str_exp, strerror(ret));

	return ret;
}

/* pthread_* functions use the reverse convention */
#define WARN_PTHREAD(exp) warn_pthread(exp, #exp)

struct flux_sem {
#ifdef _POSIX_SEMAPHORES
	sem_t sem;
#else
	pthread_mutex_t lock;
	int count;
	pthread_cond_t cond;
#endif /* _POSIX_SEMAPHORES */
};

static struct flux_sem *sem_alloc(int count)
{
	struct flux_sem *sem;

	sem = malloc(sizeof(*sem));
	if (!sem)
		return NULL;

#ifdef _POSIX_SEMAPHORES
	if (sem_init(&sem->sem, SHARE_SEM, count) < 0) {
		printf("sem_init: %s\n", strerror(errno));
		free(sem);
		return NULL;
	}
#else
	pthread_mutex_init(&sem->lock, NULL);
	sem->count = count;
	WARN_PTHREAD(pthread_cond_init(&sem->cond, NULL));
#endif /* _POSIX_SEMAPHORES */

	return sem;
}

static void sem_free(struct flux_sem *sem)
{
#ifdef _POSIX_SEMAPHORES
	WARN_UNLESS(sem_destroy(&sem->sem));
#else
	WARN_PTHREAD(pthread_cond_destroy(&sem->cond));
	WARN_PTHREAD(pthread_mutex_destroy(&sem->lock));
#endif /* _POSIX_SEMAPHORES */
	free(sem);
}

static void sem_up(struct flux_sem *sem)
{
#ifdef _POSIX_SEMAPHORES
	WARN_UNLESS(sem_post(&sem->sem));
#else
	WARN_PTHREAD(pthread_mutex_lock(&sem->lock));
	sem->count++;
	if (sem->count > 0)
		WARN_PTHREAD(pthread_cond_signal(&sem->cond));
	WARN_PTHREAD(pthread_mutex_unlock(&sem->lock));
#endif /* _POSIX_SEMAPHORES */
}

static void sem_down(struct flux_sem *sem)
{
#ifdef _POSIX_SEMAPHORES
	int err;
	do {
		err = sem_wait(&sem->sem);
	} while (err < 0 && errno == FLUX_EINTR);
	if (err < 0 && errno != FLUX_EINTR)
		printf("sem_wait: %s\n", strerror(errno));
#else
	WARN_PTHREAD(pthread_mutex_lock(&sem->lock));
	while (sem->count <= 0) {
		printf("sem_down: %lx waiting %d?\n",
		       (unsigned long)&sem->count, sem->count);
		WARN_PTHREAD(pthread_cond_wait(&sem->cond, &sem->lock));
	}
	sem->count--;
	WARN_PTHREAD(pthread_mutex_unlock(&sem->lock));
#endif /* _POSIX_SEMAPHORES */
}


struct flux_thread_wrapper_arg {
	void (*fn)(void *arg);
	void *arg;
};

void *flux_thread_wrapper(void *arg)
{
	struct flux_thread_wrapper_arg *lt = arg;
	void (*fn)(void *) = lt->fn;
	void *fn_arg = lt->arg;

	free(lt);

	/* Keep signal handling restricted to designated percpu threads. */
	if (flux_signal_block_current() < 0)
		return NULL;

	fn(fn_arg);

	return NULL;
}

static flux_thread_t thread_create(void (*fn)(void *), void *arg, char *name)
{
	pthread_t thread;
	struct flux_thread_wrapper_arg *wrapper_arg;

	wrapper_arg = malloc(sizeof(*wrapper_arg));
	if (!wrapper_arg)
		return 0;
	wrapper_arg->fn = fn;
	wrapper_arg->arg = arg;

	if (WARN_PTHREAD(pthread_create(&thread, NULL, flux_thread_wrapper,
					wrapper_arg)))
		return 0;

	return (flux_thread_t)thread;
}

static void thread_detach(void)
{
	WARN_PTHREAD(pthread_detach(pthread_self()));
}

static void thread_exit(void)
{
	pthread_exit(NULL);
}

static void thread_longjmp(int cpu)
{
	flux_thread_longjmp(cpu);
}

static int thread_join(flux_thread_t tid)
{
	if (WARN_PTHREAD(pthread_join((pthread_t)tid, NULL)))
		return -1;
	else
		return 0;
}

static flux_thread_t thread_self(void)
{
	return (flux_thread_t)pthread_self();
}

static int thread_equal(flux_thread_t a, flux_thread_t b)
{
	return pthread_equal((pthread_t)a, (pthread_t)b);
}

static void *thread_stack(unsigned long *size)
{
	pthread_attr_t thread_attr;
	size_t stack_size;
	void *thread_stack;

	if (pthread_getattr_np(pthread_self(), &thread_attr))
		return NULL;

	if (pthread_attr_getstack(&thread_attr, &thread_stack, &stack_size))
		thread_stack = NULL;

	pthread_attr_destroy(&thread_attr);

	if (size && thread_stack)
		*size = stack_size;

	return thread_stack;
}

static int thread_bind(int cpu)
{
	cpu_set_t cpu_set;

	CPU_ZERO(&cpu_set);
	CPU_SET(flux_env.cpu_list[cpu], &cpu_set);

	if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) < 0) {
		printf("sched_setaffinity: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static unsigned long long time_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return 1e9 * ts.tv_sec + ts.tv_nsec;
}


static void *timer_alloc(struct flux_timer_args *timer_args)
{
	return flux_iok_client_timer_alloc(timer_args->cpu, timer_args->oneshot);
}

static int timer_set_oneshot(void *_timer, unsigned long ns)
{
	return flux_iok_client_timer_set_oneshot(_timer, ns);
}

static void timer_free(void *_timer)
{
	/* do nothing */
}

static void host_yield(void)
{
	/*
	 * The CPU is pinned to a dedicated host CPU. Stay in userspace while
	 * idle so a timer UINTR can be delivered directly; repeatedly entering
	 * sched_yield() forces notifications through the host-kernel posted-IPI
	 * reinjection path and can lose progress in short hrtimer workloads.
	 */
	__asm__ __volatile__("pause" ::: "memory");
}

static void panic(void)
{
	exit(-1);
}

static __attribute__((no_stack_protector)) long _gettid(void)
{
	long ret;

	asm volatile("syscall"
		     : "=a"(ret)
		     : "a"(__flux__NR_gettid)
		     : "rcx", "r11", "memory");
	return ret;
}

static void *posix_malloc(unsigned long size)
{
	void *ptr = NULL;

	if (posix_memalign(&ptr, 64, (size_t)size))
		return NULL;

	return ptr;
}

static void posix_free(void *ptr)
{
	free(ptr);
}

static unsigned long page_alloc_dma_start;
static unsigned long page_alloc_dma_end;

static inline bool page_is_dma_mapping(const void *addr)
{
	unsigned long start = page_alloc_dma_start;

	return start && (unsigned long)addr >= start &&
	       (unsigned long)addr < page_alloc_dma_end;
}

#ifdef CONFIG_FLUX_FNET
static void *page_alloc_dma(void *hint, unsigned long size, int node)
{
	void *addr;

	size = align_up(size, PGSIZE_2MB);

	addr = flux_iok_client_dma_alloc(hint, size, PGSIZE_2MB, node);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to map external iok dma page\n");
		return NULL;
	}

	if (!page_alloc_dma_start ||
	    (unsigned long)addr < page_alloc_dma_start)
		page_alloc_dma_start = (unsigned long)addr;
	if ((unsigned long)addr + size > page_alloc_dma_end)
		page_alloc_dma_end = (unsigned long)addr + size;
	return addr;
}

static void page_free_dma(void *addr, unsigned long size)
{
	size = align_up(size, PGSIZE_2MB);
	flux_iok_client_dma_free(addr, size, PGSIZE_2MB);
}
#endif

static void *page_alloc_normal(void *hint, unsigned long size,
			       unsigned long align, int node)
{
	#ifdef CONFIG_FLUX_SPDK
	if (flux_env.spdk_zero_copy)
		return flux_spdk_dma_malloc(hint, size, align, node);
	else
	#endif
	{
		size_t pgsize;
		void *addr;
		int map_errno;

		if (align >= PGSIZE_1GB)
			pgsize = PGSIZE_1GB;
		else if (align >= PGSIZE_2MB)
			pgsize = PGSIZE_2MB;
		else
			pgsize = PGSIZE_4KB;

		addr = flux_mem_map_anom(hint, size, pgsize, node);
		if (addr == MAP_FAILED) {
			map_errno = errno;
			if (pgsize != PGSIZE_4KB) {
				FLUX_LOG(FLUX_LOG_WARN,
					 "page_alloc: hugepage mmap failed, falling back to 4KB pages: %s\n",
					 flux_strerror(map_errno));
				addr = flux_mem_map_anom(hint, size, PGSIZE_4KB,
							 node);
				if (addr != MAP_FAILED)
					pgsize = PGSIZE_4KB;
			}
			if (addr == MAP_FAILED) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "page_alloc: mmap failed %s\n",
					 flux_strerror(errno));
				return NULL;
			}
		}

		if (pgsize != PGSIZE_4KB && mlock(addr, size)) {
			FLUX_LOG(FLUX_LOG_ERR, "mlock failed %s\n",
				 strerror(errno));
			goto out_munmap;
		}

		return addr;
out_munmap:
		munmap(addr, size);
		return NULL;
	}
}

static void page_free_normal(void *addr, unsigned long size)
{
#ifdef CONFIG_FLUX_SPDK
	if (flux_env.spdk_zero_copy) {
		flux_spdk_dma_free(addr, size);
		return;
	}
#endif
	munmap(addr, size);
}

/* Static hugetlb backing requires full population and never falls back. */
static void *page_alloc_static_huge(void *hint, unsigned long size)
{
	unsigned char *resident;
	size_t i, pages = size / PGSIZE_4KB;
	void *addr;

	if ((unsigned long)hint % PGSIZE_2MB || !size || size % PGSIZE_2MB)
		return NULL;
	addr = flux_mem_map_anom(hint, size, PGSIZE_2MB, 0);
	if (addr == MAP_FAILED)
		return NULL;
	/*
	 * MAP_POPULATE may fail silently. Check residency before use so an
	 * undersized hugepage pool produces a controlled boot failure.
	 */
	resident = malloc(pages);
	if (!resident || mincore(addr, size, resident)) {
		free(resident);
		goto out_munmap;
	}
	for (i = 0; i < pages && (resident[i] & 1); i++)
		;
	free(resident);
	if (i != pages)
		goto out_munmap;
	FLUX_LOG(FLUX_LOG_INFO, "static hugetlb memory: %lu MiB at %p\n",
		 size >> 20, addr);
	return addr;

out_munmap:
	munmap(addr, size);
	return NULL;
}

static void *page_alloc(void *hint, unsigned long size, unsigned long align,
			int flags)
{
	int node = FLUX_PAGE_ALLOC_NODE(flags);

	if (flags & FLUX_PAGE_ALLOC_STATIC_HUGE) {
		if (node || (flags & FLUX_PAGE_ALLOC_DMA))
			return NULL;
		return page_alloc_static_huge(hint, size);
	}

#ifdef CONFIG_FLUX_FNET
	if (flux_env.fnet_enabled && FLUX_PAGE_ALLOC_IS_DMA(flags)) {
		return page_alloc_dma(hint, size, node);
	}
#endif
	return page_alloc_normal(hint, size, align, node);
}

static void page_free(void *addr, unsigned long size)
{
#ifdef CONFIG_FLUX_FNET
	if (page_is_dma_mapping(addr)) {
		page_free_dma(addr, size);
		return;
	}
#endif
	page_free_normal(addr, size);
}

static int rewrite_exec(void *addr, unsigned long len)
{
	size_t bad_offset = 0;
	int ret = flux_rewrite_exec(addr, len, &bad_offset);

	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to rewrite executable range [%p, %p) at +0x%zx\n",
			 addr, (char *)addr + len, bad_offset);
		return ret;
	}
	ret = flux_mpk_scan_exec(addr, len, &bad_offset);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rewrite produced unsafe MPK bytes in range [%p, %p) at +0x%zx\n",
			 addr, (char *)addr + len, bad_offset);
		flux_rewrite_invalidate(addr, len, true);
	}
	return ret;
}

static int invalidate_exec(void *addr, unsigned long len, bool restore)
{
	return flux_rewrite_invalidate(addr, len, restore);
}

struct flux_host_operations flux_host_ops = {
	.print = print,
	.panic = panic,
	.thread_create = thread_create,
	.thread_detach = thread_detach,
	.thread_exit = thread_exit,
	.thread_longjmp = thread_longjmp,
	.thread_join = thread_join,
	.thread_self = thread_self,
	.thread_equal = thread_equal,
	.thread_stack = thread_stack,
	.thread_bind = thread_bind,
	.sem_alloc = sem_alloc,
	.sem_free = sem_free,
	.sem_up = sem_up,
	.sem_down = sem_down,
	.time = time_ns,
	.mem_alloc = posix_malloc,
	.mem_free = posix_free,
	.page_alloc = page_alloc,
	.page_free = page_free,
	.gettid = _gettid,
	.getcpu = sched_getcpu,
	.yield = host_yield,
	.uintr_register_ipi = flux_uintr_register_ipi,
	.timer_alloc = timer_alloc,
	.timer_set_oneshot = timer_set_oneshot,
	.timer_free = timer_free,
	.handle_mpk_fault = flux_mpk_handle_fault,
	.rewrite_exec = rewrite_exec,
	.invalidate_exec = invalidate_exec,
};
