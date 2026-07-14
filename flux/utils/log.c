#include <execinfo.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <utils/log.h>

#define MAX_LOG_LEN 4096

#define COLOR_RED "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_GREEN "\033[32m"
#define COLOR_CYAN "\033[36m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_GREY "\033[90m"
#define COLOR_RESET "\033[0m"

void logk(int level, const char *fmt, ...)
{
	va_list ptr;
	off_t off;
	char buf[MAX_LOG_LEN];
	const char *color;
	bool has_newline = false;

	switch (level) {
	case FLUX_LOG_CRIT:
	case FLUX_LOG_ERR:
		color = COLOR_RED;
		break;
	case FLUX_LOG_WARN:
		color = COLOR_YELLOW;
		break;
	case FLUX_LOG_NOTICE:
		color = COLOR_MAGENTA;
		break;
	case FLUX_LOG_INFO:
		color = NULL;
		break;
	case FLUX_LOG_DEBUG:
	default:
		color = COLOR_GREY;
		break;
	}

	sprintf(buf, "[%2d] ", fast_get_cpu());

	off = strlen(buf);
	va_start(ptr, fmt);
	vsnprintf(buf + off, MAX_LOG_LEN - off, fmt, ptr);
	va_end(ptr);

	off = strlen(buf);
	if (off > 0 && buf[off - 1] == '\n') {
		has_newline = true;
		buf[off - 1] = '\0';
	}
	if (color) {
		printf("%s%s%s%s", color, buf, COLOR_RESET,
		       has_newline ? "\n" : "");
	} else {
		printf("%s%s", buf, has_newline ? "\n" : "");
	}

	fflush(stdout);
}

void logk_backtrace(void)
{
#define MAX_CALL_DEPTH 256
	void *buf[MAX_CALL_DEPTH];
	const int calls = backtrace(buf, MAX_CALL_DEPTH);
	backtrace_symbols_fd(buf, calls, 1);
}
