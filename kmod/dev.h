#ifndef _FLUX_DEV_H
#define _FLUX_DEV_H

/* Character devices are allocated dynamically at module init. */

enum {
	FLUX_DEV = 0,
	FLUX_UINTR_DEV,
	FLUX_MM_DEV,
	FLUX_LAST_DEV,
	FLUX_NR_DEVS = FLUX_LAST_DEV,
};

#define FLUX_DEV_IO_BASE 0x1000
#define FLUX_DEV_IO_UINTR_SETUP _IO(FLUX_DEV_IO_BASE, 0x1)
#define FLUX_DEV_IO_START_MAP_SHARED _IO(FLUX_DEV_IO_BASE, 0x2)
#define FLUX_DEV_IO_END_MAP_SHARED _IO(FLUX_DEV_IO_BASE, 0x3)
#define FLUX_DEV_IO_EXECVE _IO(FLUX_DEV_IO_BASE, 0x4)
#define FLUX_DEV_IO_DUMP_VMAS _IO(FLUX_DEV_IO_BASE, 0x5)
#define FLUX_DEV_IO_COPY_MM _IO(FLUX_DEV_IO_BASE, 0x6)
#define FLUX_DEV_IO_RELEASE_MM _IO(FLUX_DEV_IO_BASE, 0x7)
#define FLUX_DEV_IO_SWITCH_MM _IO(FLUX_DEV_IO_BASE, 0x8)
#define FLUX_DEV_IO_CLEAN_MM _IO(FLUX_DEV_IO_BASE, 0x9)
#define FLUX_DEV_IO_ENABLE_MPK _IO(FLUX_DEV_IO_BASE, 0xa)
#define FLUX_DEV_IO_VALIDATE_APP_RANGE _IO(FLUX_DEV_IO_BASE, 0xb)

struct flux_mpk_range {
	unsigned long start;
	unsigned long len;
};

#define FLUX_DEV_NAME "flux"
#define FLUX_UINTR_DEV_NAME FLUX_DEV_NAME "_uintr"
#define FLUX_MM_DEV_NAME FLUX_DEV_NAME "_mm"
#define FLUX_DEV_PATH "/dev/" FLUX_DEV_NAME
#define FLUX_UINTR_DEV_PATH "/dev/" FLUX_UINTR_DEV_NAME
#define FLUX_MM_DEV_PATH "/dev/" FLUX_MM_DEV_NAME

#include "uintr.h"

struct flux_percpu {
	struct uintr_percpu uintr;
};
DECLARE_PER_CPU(struct flux_percpu, flux_percpu);

struct flux_shm_percpu {
	struct uintr_upid upid;
};

struct flux_shm {
	int nr_cpus;
	int used_cpus;
	struct flux_shm_percpu pcpu[];
};
extern __read_mostly struct flux_shm *flux_shm;

struct flux_execve_args {
	char *filename;
	char **argv;
	char **envp;
};

#define FLUX_CLEAN_MM_PRE 0
#define FLUX_CLEAN_MM_POST 1
#define FLUX_CLEAN_MM_ABORT 2

#endif /* _FLUX_DEV_H */
