#ifndef _FLUX_MPK_H
#define _FLUX_MPK_H

#include <stddef.h>

struct flux_mpk_cmp64;

#ifdef CONFIG_FLUX_MPK
static inline void flux_mpk_enter_kernel(void)
{
	__asm__ __volatile__("xor %%eax, %%eax\n\t"
			     "xor %%ecx, %%ecx\n\t"
			     "xor %%edx, %%edx\n\t"
			     ".byte 0x0f, 0x01, 0xef\n\t"
			     :
			     :
			     : "eax", "ecx", "edx", "memory");
}

int flux_mpk_init(void);
int flux_mpk_protect_kernel(void *addr, size_t len, int prot);
int flux_mpk_protect_shared(void *addr, size_t len, int prot);
int flux_mpk_scan_exec(const void *addr, size_t len, size_t *offset);
int flux_mpk_handle_fault(int signum, void *ucontext,
			  struct flux_mpk_cmp64 *cmp);
#else
static inline void flux_mpk_enter_kernel(void)
{
}

static inline int flux_mpk_init(void)
{
	return 0;
}

static inline int flux_mpk_protect_kernel(void *addr, size_t len, int prot)
{
	return 0;
}

static inline int flux_mpk_scan_exec(const void *addr, size_t len,
				     size_t *offset)
{
	return 0;
}

static inline int flux_mpk_handle_fault(int signum, void *ucontext,
					struct flux_mpk_cmp64 *cmp)
{
	return 0;
}

#endif

#endif /* _FLUX_MPK_H */
