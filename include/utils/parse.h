#ifndef _UTILS_PARSE_H
#define _UTILS_PARSE_H

#include <utils/base.h>

/*
 * Parse an exact, non-NUL-terminated ASCII span without consulting locale,
 * allocating memory, setting errno, or acquiring libc-internal locks.
 * A base of zero accepts decimal and a 0x-prefixed hexadecimal value.
 */
bool flux_parse_u64_ascii(const char *first, const char *last,
			  unsigned int base, uint64_t *value);
bool flux_parse_i64_ascii(const char *first, const char *last,
			  unsigned int base, int64_t *value);

#endif /* _UTILS_PARSE_H */
