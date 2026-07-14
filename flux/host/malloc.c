#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <flux.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define FLUX_SHARED_ALLOC_SIZE (512UL << 20)
#define FLUX_SHARED_ALLOC_BASE \
	(FLUX_MEMORY_ADDR - FLUX_SHARED_ALLOC_SIZE)
#define FLUX_SHARED_ALLOC_MAGIC 0x46584d414c4c4f43ULL
#define FLUX_SHARED_ALLOC_PAGESIZE 4096UL

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t nmemb, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void __libc_free(void *ptr);
extern void *__libc_memalign(size_t alignment, size_t size);

struct flux_shared_alloc_meta {
	uint64_t magic;
	size_t requested_size;
	size_t usable_size;
};

struct flux_shared_allocator {
	void *base;
	size_t len;
	uintptr_t next;
	volatile int lock;
	bool inited;
};

static struct flux_shared_allocator flux_shared_alloc;
static volatile int flux_shared_alloc_warn_once;

static void flux_shared_alloc_diag(const char *op, const void *ptr)
{
	char buf[160];
	uintptr_t value = (uintptr_t)ptr;
	size_t pos;

	if (__sync_lock_test_and_set(&flux_shared_alloc_warn_once, 1))
		return;

	pos = (size_t)snprintf(buf, sizeof(buf),
			       "flux: suspicious shared allocator %s ptr=0x%lx arena=[0x%lx,0x%lx)\n",
			       op, (unsigned long)value,
			       (unsigned long)FLUX_SHARED_ALLOC_BASE,
			       (unsigned long)(FLUX_SHARED_ALLOC_BASE +
					       FLUX_SHARED_ALLOC_SIZE));
	if (pos > sizeof(buf))
		pos = sizeof(buf);

	{
		ssize_t wr __attribute__((unused));

		wr = write(STDERR_FILENO, buf, pos);
	}
}

static inline void flux_shared_alloc_lock(void)
{
	while (__sync_lock_test_and_set(&flux_shared_alloc.lock, 1)) {
		while (__atomic_load_n(&flux_shared_alloc.lock, __ATOMIC_RELAXED))
			__asm__ __volatile__("pause");
	}
}

static inline void flux_shared_alloc_unlock(void)
{
	__sync_lock_release(&flux_shared_alloc.lock);
}

static inline bool flux_shared_alloc_contains(const void *ptr)
{
	uintptr_t addr = (uintptr_t)ptr;
	uintptr_t base = (uintptr_t)flux_shared_alloc.base;

	return flux_shared_alloc.inited && addr >= base &&
	       addr < base + flux_shared_alloc.len;
}

static inline bool flux_shared_alloc_is_pow2(size_t value)
{
	return value && !(value & (value - 1));
}

static inline size_t flux_shared_alloc_default_alignment(void)
{
	return alignof(max_align_t);
}

static inline uintptr_t flux_shared_alloc_align_up(uintptr_t addr, size_t align)
{
	return (addr + align - 1) & ~(uintptr_t)(align - 1);
}

static struct flux_shared_alloc_meta *flux_shared_alloc_ptr_meta(const void *ptr)
{
	struct flux_shared_alloc_meta *meta;

	if (!ptr || !flux_shared_alloc_contains(ptr))
		return NULL;

	meta = (struct flux_shared_alloc_meta *)((uintptr_t)ptr - sizeof(*meta));
	if (meta->magic != FLUX_SHARED_ALLOC_MAGIC)
		return NULL;

	return meta;
}

/*
 * Keep the post-hook allocator deliberately simple: allocations are monotonic
 * within the fixed shared arena, and shared frees are treated as no-ops.
 * This avoids free-list corruption while preserving stable shared addresses.
 */
static void *flux_shared_alloc_alloc_locked(size_t size, size_t alignment)
{
	struct flux_shared_alloc_meta *meta;
	uintptr_t payload;
	uintptr_t alloc_end;
	uintptr_t arena_end = (uintptr_t)flux_shared_alloc.base +
			      flux_shared_alloc.len;
	size_t min_align = flux_shared_alloc_default_alignment();

	if (!size)
		size = 1;

	if (alignment < min_align)
		alignment = min_align;
	if (!flux_shared_alloc_is_pow2(alignment))
		return NULL;

	payload = flux_shared_alloc_align_up(flux_shared_alloc.next +
					       sizeof(*meta),
				       alignment);
	if (__builtin_add_overflow(payload, size, &alloc_end))
		return NULL;

	alloc_end = flux_shared_alloc_align_up(alloc_end, min_align);
	if (alloc_end > arena_end)
		return NULL;

	meta = (struct flux_shared_alloc_meta *)(payload - sizeof(*meta));
	meta->magic = FLUX_SHARED_ALLOC_MAGIC;
	meta->requested_size = size;
	meta->usable_size = alloc_end - payload;
	flux_shared_alloc.next = alloc_end;
	return (void *)payload;
}

int flux_malloc_hooks_init(void)
{
	void *addr;

	if (flux_shared_alloc.inited)
		return 0;

	addr = mmap((void *)FLUX_SHARED_ALLOC_BASE, FLUX_SHARED_ALLOC_SIZE,
		    PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (addr == MAP_FAILED || addr != (void *)FLUX_SHARED_ALLOC_BASE) {
		if (addr != MAP_FAILED && addr != (void *)FLUX_SHARED_ALLOC_BASE)
			munmap(addr, FLUX_SHARED_ALLOC_SIZE);
		return -1;
	}

	flux_shared_alloc.base = addr;
	flux_shared_alloc.len = FLUX_SHARED_ALLOC_SIZE;
	flux_shared_alloc.next = (uintptr_t)addr;
	flux_shared_alloc.lock = 0;
	flux_shared_alloc.inited = true;

	return 0;
}

void flux_malloc_hooks_enable(void)
{
	flux_env.malloc_hook_enabled = true;
}

void flux_malloc_hooks_disable(void)
{
	flux_env.malloc_hook_enabled = false;
}

void *malloc(size_t size)
{
	void *ptr;

	if (!flux_env.malloc_hook_enabled || !flux_shared_alloc.inited)
		return __libc_malloc(size);

	flux_shared_alloc_lock();
	ptr = flux_shared_alloc_alloc_locked(size,
					    flux_shared_alloc_default_alignment());
	flux_shared_alloc_unlock();
	if (!ptr)
		errno = FLUX_ENOMEM;
	return ptr;
}

void free(void *ptr)
{
	struct flux_shared_alloc_meta *meta;

	if (!ptr)
		return;

	meta = flux_shared_alloc_ptr_meta(ptr);
	if (meta) {
		meta->magic = 0;
		return;
	}
	if (flux_shared_alloc_contains(ptr)) {
		flux_shared_alloc_diag("free", ptr);
		return;
	}

	__libc_free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
	void *ptr;
	size_t total;

	if (__builtin_mul_overflow(nmemb, size, &total)) {
		errno = FLUX_ENOMEM;
		return NULL;
	}

	if (!flux_env.malloc_hook_enabled || !flux_shared_alloc.inited)
		return __libc_calloc(nmemb, size);

	flux_shared_alloc_lock();
	ptr = flux_shared_alloc_alloc_locked(total,
					    flux_shared_alloc_default_alignment());
	flux_shared_alloc_unlock();
	if (!ptr) {
		errno = FLUX_ENOMEM;
		return NULL;
	}

	memset(ptr, 0, total);
	return ptr;
}

void *realloc(void *ptr, size_t size)
{
	struct flux_shared_alloc_meta *meta;
	void *new_ptr;

	if (!ptr)
		return malloc(size);

	if (!size) {
		free(ptr);
		return NULL;
	}

	meta = flux_shared_alloc_ptr_meta(ptr);
	if (!meta) {
		if (flux_shared_alloc_contains(ptr)) {
			flux_shared_alloc_diag("realloc", ptr);
			errno = FLUX_EINVAL;
			return NULL;
		}
		return __libc_realloc(ptr, size);
	}

	if (size <= meta->usable_size) {
		meta->requested_size = size;
		return ptr;
	}

	new_ptr = malloc(size);
	if (!new_ptr)
		return NULL;

	memcpy(new_ptr, ptr, meta->requested_size);
	meta->magic = 0;
	return new_ptr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *ptr;

	if (!flux_shared_alloc_is_pow2(alignment) ||
	    alignment < sizeof(void *))
		return FLUX_EINVAL;

	if (!flux_env.malloc_hook_enabled || !flux_shared_alloc.inited) {
		ptr = __libc_memalign(alignment, size);
		if (!ptr)
			return FLUX_ENOMEM;
		*memptr = ptr;
		return 0;
	}

	flux_shared_alloc_lock();
	ptr = flux_shared_alloc_alloc_locked(size, alignment);
	flux_shared_alloc_unlock();
	if (!ptr)
		return FLUX_ENOMEM;

	*memptr = ptr;
	return 0;
}

void *aligned_alloc(size_t alignment, size_t size)
{
	void *ptr = NULL;

	if (!flux_shared_alloc_is_pow2(alignment) ||
	    (size & (alignment - 1))) {
		errno = FLUX_EINVAL;
		return NULL;
	}

	{
		int err = posix_memalign(&ptr, alignment, size);

		if (err) {
			errno = err;
			return NULL;
		}
	}

	return ptr;
}

void *memalign(size_t alignment, size_t size)
{
	void *ptr = NULL;
	int err = posix_memalign(&ptr, alignment, size);

	if (err) {
		errno = err;
		return NULL;
	}

	return ptr;
}

void *valloc(size_t size)
{
	void *ptr = NULL;
	long page_size = sysconf(_SC_PAGESIZE);

	if (page_size <= 0)
		page_size = FLUX_SHARED_ALLOC_PAGESIZE;

	if (posix_memalign(&ptr, (size_t)page_size, size)) {
		errno = FLUX_ENOMEM;
		return NULL;
	}

	return ptr;
}

void *pvalloc(size_t size)
{
	void *ptr = NULL;
	size_t rounded;
	long page_size = sysconf(_SC_PAGESIZE);

	if (page_size <= 0)
		page_size = FLUX_SHARED_ALLOC_PAGESIZE;

	if (__builtin_add_overflow(size, (size_t)page_size - 1, &rounded)) {
		errno = FLUX_ENOMEM;
		return NULL;
	}
	rounded &= ~((size_t)page_size - 1);

	if (posix_memalign(&ptr, (size_t)page_size, rounded)) {
		errno = FLUX_ENOMEM;
		return NULL;
	}

	return ptr;
}
