#ifndef _ASM_FLUX_HOST_DEV_H
#define _ASM_FLUX_HOST_DEV_H

#include <linux/ioctl.h>
#include <asm/syscalls.h>

#define FLUX_DEV_IO_BASE 0x1000
#define FLUX_DEV_IO_COPY_MM _IO(FLUX_DEV_IO_BASE, 0x6)
#define FLUX_DEV_IO_RELEASE_MM _IO(FLUX_DEV_IO_BASE, 0x7)
#define FLUX_DEV_IO_SWITCH_MM _IO(FLUX_DEV_IO_BASE, 0x8)
#define FLUX_DEV_IO_CLEAN_MM _IO(FLUX_DEV_IO_BASE, 0x9)
#define FLUX_DEV_IO_ENABLE_MPK _IO(FLUX_DEV_IO_BASE, 0xa)
#define FLUX_DEV_IO_VALIDATE_APP_RANGE _IO(FLUX_DEV_IO_BASE, 0xb)

#define FLUX_DEV_NAME "flux"
#define FLUX_MM_DEV_NAME FLUX_DEV_NAME "_mm"
#define FLUX_MM_DEV_PATH "/dev/" FLUX_MM_DEV_NAME

extern int flux_host_dev_fd;

extern void flux_host_dev_init(void);
extern void flux_host_dev_exit(void);

#define flux_host_dev_call_mm(cmd, arg) \
	host_syscall(__NR_ioctl, flux_host_dev_fd, cmd, arg)

static inline int flux_host_dev_copy_mm(void)
{
	int proc_key, err;

	err = flux_host_dev_call_mm(FLUX_DEV_IO_COPY_MM, &proc_key);
	if (err < 0)
		return err;

	return proc_key;
}

static inline int flux_host_dev_release_mm(int proc_key)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_RELEASE_MM, proc_key);
}

static inline int flux_host_dev_switch_mm(int proc_key_to, int proc_key_from)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_SWITCH_MM,
				     ((u64)proc_key_to << 32) | proc_key_from);
}

#define FLUX_CLEAN_MM_PRE 0
#define FLUX_CLEAN_MM_POST 1
#define FLUX_CLEAN_MM_ABORT 2

static inline int flux_host_dev_clean_mm_pre(void)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_CLEAN_MM, FLUX_CLEAN_MM_PRE);
}

static inline int flux_host_dev_clean_mm_post(void)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_CLEAN_MM, FLUX_CLEAN_MM_POST);
}

static inline int flux_host_dev_clean_mm_abort(void)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_CLEAN_MM, FLUX_CLEAN_MM_ABORT);
}

static inline int flux_host_dev_enable_mpk(void)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_ENABLE_MPK, 0);
}

struct flux_mpk_range {
	unsigned long start;
	unsigned long len;
};

static inline int
flux_host_dev_validate_app_range(unsigned long start, unsigned long len)
{
	struct flux_mpk_range range = {
		.start = start,
		.len = len,
	};

	return flux_host_dev_call_mm(FLUX_DEV_IO_VALIDATE_APP_RANGE, &range);
}

#endif /* _ASM_FLUX_HOST_DEV_H */