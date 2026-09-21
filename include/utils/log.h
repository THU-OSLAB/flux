#ifndef _UTILS_LOG_H
#define _UTILS_LOG_H

#include <utils/base.h>

/* Logging levels shared across the host runtime and helper libraries. */
enum flux_log_level {
	FLUX_LOG_CRIT = 1,
	FLUX_LOG_ERR = 2,
	FLUX_LOG_WARN = 3,
	FLUX_LOG_NOTICE = 4,
	FLUX_LOG_INFO = 5,
	FLUX_LOG_DEBUG = 6,
};

extern void logk(int level, const char *fmt, ...)
	__attribute__((__format__(__printf__, 2, 3)));
extern void logk_backtrace(void);
extern bool flux_stdout_needs_crlf(void);

#if defined(LOG_LEVEL_DEBUG) || defined(DEBUG)
#define MAX_LOG_LEVEL FLUX_LOG_DEBUG
#elif defined(LOG_LEVEL_INFO)
#define MAX_LOG_LEVEL FLUX_LOG_INFO
#elif defined(LOG_LEVEL_NOTICE)
#define MAX_LOG_LEVEL FLUX_LOG_NOTICE
#elif defined(LOG_LEVEL_WARN)
#define MAX_LOG_LEVEL FLUX_LOG_WARN
#elif defined(LOG_LEVEL_ERR)
#define MAX_LOG_LEVEL FLUX_LOG_ERR
#elif defined(LOG_LEVEL_CRIT)
#define MAX_LOG_LEVEL FLUX_LOG_CRIT
#else
#define MAX_LOG_LEVEL FLUX_LOG_INFO
#endif

/*
 * Per-translation-unit log prefix. Source files may override FLUX_FMT before
 * including a public umbrella header.
 */
#ifndef FLUX_FMT
#define FLUX_FMT ""
#endif

#define FLUX_LOG(level, fmt, ...)                                      \
	do {                                                          \
		if (level <= MAX_LOG_LEVEL)                           \
			logk(level, "<%s:%d> " FLUX_FMT fmt, __FILE__, \
			     __LINE__, ##__VA_ARGS__);                \
	} while (0)

#endif /* _UTILS_LOG_H */
