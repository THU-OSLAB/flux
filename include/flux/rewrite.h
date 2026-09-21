#ifndef _FLUX_REWRITE_H
#define _FLUX_REWRITE_H

#include <stdbool.h>
#include <stddef.h>

int flux_rewrite_init(void);
/*
 * The LibOS MM caller owns mapping permissions and serialization. Rewriting
 * requires private, writable, NX bytes; restoring requires writable bytes.
 * These operations never inspect or change application mappings themselves.
 */
int flux_rewrite_exec(void *addr, size_t len, size_t *offset);
int flux_rewrite_invalidate(void *addr, size_t len, bool restore);
#endif /* _FLUX_REWRITE_H */
