#ifndef _UTILS_PATH_H
#define _UTILS_PATH_H

#include <stddef.h>

/*
 * Allocate a host path scoped to the current user, formatted as
 * "<base>.<user>". Falls back to the numeric uid when the username cannot be
 * resolved.
 */
char *flux_user_scoped_path_strdup(const char *base);

#endif /* _UTILS_PATH_H */
