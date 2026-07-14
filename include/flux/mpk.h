#ifndef _FLUX_MPK_H
#define _FLUX_MPK_H

#include <stddef.h>

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
int flux_mpk_disable_host_rseq(void);
int flux_mpk_protect_app(void *addr, size_t len, int prot);
int flux_mpk_protect_shared(void *addr, size_t len, int prot);
int flux_mpk_scan_binary(const void *addr, size_t len, size_t *offset);
#else
static inline void flux_mpk_enter_kernel(void)
{
}

static inline int flux_mpk_init(void)
{
	return 0;
}

static inline int flux_mpk_disable_host_rseq(void)
{
	return 0;
}

static inline int flux_mpk_protect_app(void *addr, size_t len, int prot)
{
	return 0;
}

static inline int flux_mpk_scan_binary(const void *addr, size_t len,
				       size_t *offset)
{
	return 0;
}

#endif

#endif /* _FLUX_MPK_H */
