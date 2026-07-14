#ifndef _UTILS_SYSCTL_H
#define _UTILS_SYSCTL_H

#include <utils/base.h>

/* Write a single sysctl entry addressed in dotted notation. */
int flux_sysctl(const char *path, const char *value);

/* Apply a semicolon-separated list of key=value sysctl assignments. */
void flux_sysctl_parse_write(const char *sysctls);

#endif /* _UTILS_SYSCTL_H */
