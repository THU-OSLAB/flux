#ifndef _UTILS_BASE_H
#define _UTILS_BASE_H

#include <assert.h>
#include <bits/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

/*
 * Low-level helpers shared across the userspace runtime.
 *
 * Keep this header dependency-light so internal headers can include it
 * without pulling in the full Flux public API surface.
 */

#define CACHE_LINE_SIZE 64
#define RSP_ALIGNMENT 16

#define check_type(expr, type) ((typeof(expr) *)0 != (type *)0)
#define check_types_match(expr1, expr2) \
	((typeof(expr1) *)0 != (typeof(expr2) *)0)

/**
 * container_of - get pointer to enclosing structure
 * @member_ptr: pointer to the structure member
 * @containing_type: the type this member is within
 * @member: the name of this member within the structure
 *
 * Given a pointer to a member of a structure, return the pointer to the
 * enclosing object.
 */
#ifndef container_of
#define container_of(member_ptr, containing_type, member)         \
	((containing_type *)((char *)(member_ptr) -               \
			     offsetof(containing_type, member)) + \
	 check_types_match(*(member_ptr), ((containing_type *)0)->member))
#endif

/**
 * MAX - picks the maximum of two expressions
 *
 * Arguments @a and @b are evaluated exactly once.
 */
#define MAX(a, b)                   \
	({                          \
		typeof(a) _a = (a); \
		typeof(b) _b = (b); \
		_a > _b ? _a : _b;  \
	})

/**
 * MIN - picks the minimum of two expressions
 *
 * Arguments @a and @b are evaluated exactly once.
 */
#define MIN(a, b)                   \
	({                          \
		typeof(a) _a = (a); \
		typeof(b) _b = (b); \
		_a < _b ? _a : _b;  \
	})

/**
 * is_power_of_two - determines if an integer is a power of two
 * @x: the value
 *
 * Returns true if the integer is a power of two.
 */
#define is_power_of_two(x) ((x) != 0 && !((x) & ((x) - 1)))

/**
 * align_up - rounds a value up to an alignment
 * @x: the value
 * @align: the alignment (must be power of 2)
 *
 * Returns an aligned value.
 */
#define align_up(x, align)                                  \
	({                                                  \
		assert(is_power_of_two(align));             \
		(((x) - 1) | ((typeof(x))(align) - 1)) + 1; \
	})

/**
 * align_down - rounds a value down to an alignment
 * @x: the value
 * @align: the alignment (must be power of 2)
 *
 * Returns an aligned value.
 */
#define align_down(x, align)                       \
	({                                         \
		assert(is_power_of_two(align));    \
		((x) & ~((typeof(x))(align) - 1)); \
	})

/**
 * is_aligned - determines if a value is aligned
 * @x: the value
 * @align: the alignment (must be power of 2)
 *
 * Returns true if the value is aligned.
 */
#define is_aligned(x, align) (((x) & ((typeof(x))(align) - 1)) == 0)

/**
 * div_up - divides two numbers, rounding up to an integer
 * @x: the dividend
 * @d: the divisor
 *
 * Returns a rounded-up quotient.
 */
#define div_up(x, d) ((((x) + (d) - 1)) / (d))

#define KB (1024)
#define MB (1024 * 1024)
#define GB (1UL * 1024 * 1024 * 1024)

#define PGSHIFT_4KB 12
#define PGSHIFT_2MB 21
#define PGSHIFT_1GB 30

#define PGSIZE_4KB (1 << PGSHIFT_4KB)
#define PGSIZE_2MB (1 << PGSHIFT_2MB)
#define PGSIZE_1GB (1 << PGSHIFT_1GB)

#define PGMASK_4KB (PGSIZE_4KB - 1)
#define PGMASK_2MB (PGSIZE_2MB - 1)
#define PGMASK_1GB (PGSIZE_1GB - 1)

#define PGN_4KB(la) (((uintptr_t)(la)) >> PGSHIFT_4KB)
#define PGN_2MB(la) (((uintptr_t)(la)) >> PGSHIFT_2MB)
#define PGN_1GB(la) (((uintptr_t)(la)) >> PGSHIFT_1GB)

#define PGOFF_4KB(la) (((uintptr_t)(la)) & PGMASK_4KB)
#define PGOFF_2MB(la) (((uintptr_t)(la)) & PGMASK_2MB)
#define PGOFF_1GB(la) (((uintptr_t)(la)) & PGMASK_1GB)

#define PGADDR_4KB(la) (((uintptr_t)(la)) & ~((uintptr_t)PGMASK_4KB))
#define PGADDR_2MB(la) (((uintptr_t)(la)) & ~((uintptr_t)PGMASK_2MB))
#define PGADDR_1GB(la) (((uintptr_t)(la)) & ~((uintptr_t)PGMASK_1GB))

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define unreachable() __builtin_unreachable()

#define prefetch0(x) __builtin_prefetch((x), 0, 3)
#define prefetch1(x) __builtin_prefetch((x), 0, 2)
#define prefetch2(x) __builtin_prefetch((x), 0, 1)
#define prefetchnta(x) __builtin_prefetch((x), 0, 0)
#define prefetch(x) prefetch0(x)

/* Common variable attributes used by runtime-facing UAPI structs. */
#define __packed __attribute__((packed))
#define __notused __attribute__((unused))
#define __used __attribute__((used))
#define __aligned(x) __attribute__((aligned(x)))

/* Common function attributes used throughout the userspace runtime. */
#define __api
#define __noinline __attribute__((noinline))
#define __noreturn __attribute__((noreturn))
#define __must_use_return __attribute__((warn_unused_result))
#define __pure __attribute__((pure))
#define __weak __attribute__((weak))
#define __malloc __attribute__((malloc))
#define __assume_aligned(x) __attribute__((assume_aligned(x)))

#define barrier() asm volatile("" ::: "memory")

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#define atomic_load_relaxed(ptr) __atomic_load_n((ptr), __ATOMIC_RELAXED)
#define atomic_load_acquire(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)

#define atomic_store_relaxed(ptr, val) \
	__atomic_store_n((ptr), (val), __ATOMIC_RELAXED)
#define atomic_store_release(ptr, val) \
	__atomic_store_n((ptr), (val), __ATOMIC_RELEASE)

#define atomic_cmpxchg_relaxed(ptr, expected, desired)                  \
	__atomic_compare_exchange_n((ptr), (expected), (desired), false, \
				    __ATOMIC_RELAXED, __ATOMIC_RELAXED)
#define atomic_cmpxchg_release_relaxed(ptr, expected, desired)          \
	__atomic_compare_exchange_n((ptr), (expected), (desired), false, \
				    __ATOMIC_RELEASE, __ATOMIC_RELAXED)
#define atomic_cmpxchg_acq_rel_acquire(ptr, expected, desired)          \
	__atomic_compare_exchange_n((ptr), (expected), (desired), false, \
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#define type_is_native(t)                                           \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#define type_is_pointer(t) \
	(__builtin_types_compatible_p(typeof(t), typeof(&*t)))

static __always_inline void __write_once_size(volatile void *p, void *res,
					      int size)
{
	switch (size) {
	case 1:
		*(volatile uint8_t *)p = *(uint8_t *)res;
		break;
	case 2:
		*(volatile uint16_t *)p = *(uint16_t *)res;
		break;
	case 4:
		*(volatile uint32_t *)p = *(uint32_t *)res;
		break;
	case 8:
		*(volatile uint64_t *)p = *(uint64_t *)res;
		break;
	default:
		barrier();
		__builtin_memcpy((void *)p, (const void *)res, size);
		barrier();
	}
}

#define WRITE_ONCE(x, val)                                   \
	({                                                   \
		union {                                      \
			typeof(x) __val;                     \
			char __c[1];                         \
		} __u = { .__val = (typeof(x))(val) };       \
		__write_once_size(&(x), __u.__c, sizeof(x)); \
		__u.__val;                                   \
	})

#define VDSO_CPUNODE_BITS 12
#define VDSO_CPUNODE_MASK 0xfff

static __always_inline int fast_get_cpu(void)
{
	unsigned int eax, ecx, edx;
	asm volatile("rdtscp" : "=a"(eax), "=d"(edx), "=c"(ecx));
	return ecx & VDSO_CPUNODE_MASK;
}

static __always_inline int fast_get_node(void)
{
	unsigned int eax, ecx, edx;
	asm volatile("rdtscp" : "=a"(eax), "=d"(edx), "=c"(ecx));
	return ecx >> VDSO_CPUNODE_BITS;
}

#endif /* _UTILS_BASE_H */
