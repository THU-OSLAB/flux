#ifndef _FLUX_HOST_KMOD_H
#define _FLUX_HOST_KMOD_H

#include <stdint.h>

#include <kernel/linux/ioctl.h>
#include <utils/base.h>

#define FLUX_DEV_IO_BASE 0x1000
#define FLUX_DEV_IO_UINTR_SETUP _FLUX_IO(FLUX_DEV_IO_BASE, 0x1)
#define FLUX_DEV_IO_START_MAP_SHARED _FLUX_IO(FLUX_DEV_IO_BASE, 0x2)
#define FLUX_DEV_IO_END_MAP_SHARED _FLUX_IO(FLUX_DEV_IO_BASE, 0x3)
#define FLUX_DEV_IO_EXECVE _FLUX_IO(FLUX_DEV_IO_BASE, 0x4)
#define FLUX_DEV_IO_DUMP_VMAS _FLUX_IO(FLUX_DEV_IO_BASE, 0x5)
#define FLUX_DEV_IO_COPY_MM _FLUX_IO(FLUX_DEV_IO_BASE, 0x6)
#define FLUX_DEV_IO_RELEASE_MM _FLUX_IO(FLUX_DEV_IO_BASE, 0x7)
#define FLUX_DEV_IO_CLEAN_MM_PRE _FLUX_IO(FLUX_DEV_IO_BASE, 0x9)
#define FLUX_DEV_IO_CLEAN_MM_POST _FLUX_IO(FLUX_DEV_IO_BASE, 0xa)

#define FLUX_DEV_NAME "flux"
#define FLUX_UINTR_DEV_NAME FLUX_DEV_NAME "_uintr"
#define FLUX_MM_DEV_NAME FLUX_DEV_NAME "_mm"
#define FLUX_DEV_PATH "/dev/" FLUX_DEV_NAME
#define FLUX_UINTR_DEV_PATH "/dev/" FLUX_UINTR_DEV_NAME
#define FLUX_MM_DEV_PATH "/dev/" FLUX_MM_DEV_NAME

struct uintr_upid {
	union {
		struct {
			uint8_t status; /* bit 0: ON, bit 1: SN, bit 2-7: reserved */
			uint8_t reserved1; /* Reserved */
			uint8_t nv; /* Notification vector */
			uint8_t reserved2; /* Reserved */
			uint32_t ndst; /* Notification destination */
		} nc __packed; /* Notification control */
		long unsigned int word_val;
	};
	uint64_t puir; /* Posted user interrupt requests */
} __aligned(64);

struct flux_shm_percpu {
	struct uintr_upid upid;
};

struct flux_shm {
	int nr_cpus;
	int used_cpus;
	struct flux_shm_percpu per_cpu[];
};

extern struct flux_shm *flux_shm;

struct flux_execve_args {
	char *filename;
	char **argv;
	char **envp;
};

#endif /* _FLUX_HOST_KMOD_H */
