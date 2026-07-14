#define FLUX_FMT "iokd-kmod: "

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <utils/base.h>
#include <utils/log.h>

#include "iokd.h"

struct flux_shm *flux_shm;

static __thread int flux_iokd_uintr_fd = -1;

int flux_iokd_kmod_map_shm(void)
{
	int fd;
	void *addr;

	fd = open(FLUX_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n", FLUX_DEV_PATH);
		return -1;
	}

	addr = mmap(NULL, PGSIZE_2MB, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    0);
	close(fd);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mmap %s shared memory\n",
			 FLUX_DEV_PATH);
		return -1;
	}

	flux_shm = addr;
	FLUX_LOG(FLUX_LOG_INFO, "mapped %s shared memory at %p\n",
		 FLUX_DEV_PATH, addr);
	return 0;
}

int flux_iokd_kmod_init_sender(void)
{
	int fd;

#ifndef CONFIG_FLUX_UINTR
	return 0;
#endif

	if (flux_iokd_uintr_fd >= 0)
		return 0;

	fd = open(FLUX_UINTR_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n",
			 FLUX_UINTR_DEV_PATH);
		return -1;
	}

	if (ioctl(fd, FLUX_DEV_IO_UINTR_SETUP, NULL) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to initialize uintr sender state\n");
		close(fd);
		return -1;
	}

	flux_iokd_uintr_fd = fd;
	FLUX_LOG(FLUX_LOG_INFO, "initialized %s sender state\n",
		 FLUX_UINTR_DEV_PATH);
	return 0;
}

void flux_iokd_kmod_fini_sender(void)
{
#ifdef CONFIG_FLUX_UINTR
	if (flux_iokd_uintr_fd >= 0) {
		FLUX_LOG(FLUX_LOG_INFO, "closing %s sender state fd=%d\n",
			 FLUX_UINTR_DEV_PATH, flux_iokd_uintr_fd);
		close(flux_iokd_uintr_fd);
		flux_iokd_uintr_fd = -1;
	}
#endif
}

void flux_iokd_kmod_unmap_shm(void)
{
	if (flux_shm) {
		FLUX_LOG(FLUX_LOG_INFO, "unmapping %s shared memory at %p\n",
			 FLUX_DEV_PATH, flux_shm);
		munmap(flux_shm, PGSIZE_2MB);
		flux_shm = NULL;
	}
}
