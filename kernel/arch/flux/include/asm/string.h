/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_STRING_H
#define _ASM_FLUX_STRING_H

#include <asm/types.h>
#include <asm/host_ops.h>

#define __HAVE_ARCH_MEMCPY 1
void *memcpy(void *to, const void *from ,size_t len);
void *__memcpy(void *to, const void *from ,size_t len);

#define __HAVE_ARCH_MEMSET
void *memset(void *s, int c, size_t n);
void *__memset(void *s, int c, size_t n);

/*
 * KMSAN needs to instrument as much code as possible. Use C versions of
 * memsetXX() from lib/string.c under KMSAN.
 */
#if !defined(CONFIG_KMSAN)
#define __HAVE_ARCH_MEMSET16
static inline void *memset16(uint16_t *s, uint16_t v, size_t n)
{
	long d0, d1;
	asm volatile("rep\n\t"
		     "stosw"
		     : "=&c" (d0), "=&D" (d1)
		     : "a" (v), "1" (s), "0" (n)
		     : "memory");
	return s;
}

#define __HAVE_ARCH_MEMSET32
static inline void *memset32(uint32_t *s, uint32_t v, size_t n)
{
	long d0, d1;
	asm volatile("rep\n\t"
		     "stosl"
		     : "=&c" (d0), "=&D" (d1)
		     : "a" (v), "1" (s), "0" (n)
		     : "memory");
	return s;
}

#define __HAVE_ARCH_MEMSET64
static inline void *memset64(uint64_t *s, uint64_t v, size_t n)
{
	long d0, d1;
	asm volatile("rep\n\t"
		     "stosq"
		     : "=&c" (d0), "=&D" (d1)
		     : "a" (v), "1" (s), "0" (n)
		     : "memory");
	return s;
}
#endif

#define __HAVE_ARCH_MEMMOVE
void *memmove(void *dest, const void *src, size_t count);
void *__memmove(void *dest, const void *src, size_t count);

int memcmp(const void *cs, const void *ct, size_t count);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
int strcmp(const char *cs, const char *ct);

#endif /* _ASM_FLUX_STRING_H */
