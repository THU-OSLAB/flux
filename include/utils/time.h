#ifndef _UTILS_TIME_H
#define _UTILS_TIME_H

#include <time.h>

#include <utils/base.h>

#define NSEC_PER_SEC (1000000000UL)
#define NSEC_PER_MSEC (1000000UL)
#define NSEC_PER_USEC (1000UL)
#define USEC_PER_SEC (1000000UL)
#define USEC_PER_MSEC (1000UL)
#define MSEC_PER_SEC (1000UL)

static inline unsigned long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static inline unsigned long now_tsc(void)
{
	unsigned int lo, hi;

	asm volatile("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
	return ((unsigned long)hi << 32) | lo;
}

#endif
