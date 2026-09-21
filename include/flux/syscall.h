#ifndef _FLUX_SYSCALL_H
#define _FLUX_SYSCALL_H

#include <flux/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convenience wrappers around the generated Flux syscall entry points.
 *
 * They do not change syscall semantics; they only encode common call patterns
 * used throughout the userspace runtime.
 */
static inline long flux_sys_stat(const char *path, struct flux_stat *buf)
{
	return flux_sys_newfstatat(FLUX_AT_FDCWD, path, buf, 0);
}

static inline long flux_sys_lstat(const char *path, struct flux_stat *buf)
{
	return flux_sys_newfstatat(FLUX_AT_FDCWD, path, buf,
				   FLUX_AT_SYMLINK_NOFOLLOW);
}

static inline long flux_sys_fstat(int fd, struct flux_stat *buf)
{
	return flux_sys_newfstatat(fd, "", buf, FLUX_AT_EMPTY_PATH);
}

#ifdef CONFIG_FLUX_FNET
static inline long flux_sys_send(int fd, void *buf, size_t len, int flags)
{
	return flux_sys_sendto(fd, buf, len, flags, 0, 0);
}
#endif

#ifdef CONFIG_FLUX_FNET
static inline long flux_sys_recv(int fd, void *buf, size_t len, int flags)
{
	return flux_sys_recvfrom(fd, buf, len, flags, 0, 0);
}
#endif

/*
 * Halt the Flux kernel and return control to the host runtime.
 */
static inline long flux_sys_halt(void)
{
	return flux_sys_reboot(FLUX_LINUX_REBOOT_MAGIC1,
			       FLUX_LINUX_REBOOT_MAGIC2,
			       FLUX_LINUX_REBOOT_CMD_POWER_OFF, NULL);
}

#ifdef __cplusplus
}
#endif

#endif
