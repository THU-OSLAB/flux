#ifndef _UTILS_ERROR_H
#define _UTILS_ERROR_H

#include <utils/base.h>

/* Convert Flux-style negative errno values into readable diagnostics. */
const char *flux_strerror(int err);

/* Emit a prefixed error message through the common runtime logger. */
void flux_perror(char *msg, int err);

#endif /* _UTILS_ERROR_H */
