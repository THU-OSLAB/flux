#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <flux.h>

#include "kmod.h"

struct flux_shm *flux_shm;

#ifdef CONFIG_FLUX_UINTR
static __thread int flux_uintr_fd = -1;
#endif

int flux_kmod_init_percpu(void *handler)
{
#ifdef CONFIG_FLUX_UINTR
	int err, fd;

	if (flux_uintr_fd >= 0)
		return 0;

	fd = open(FLUX_UINTR_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n",
			 FLUX_UINTR_DEV_PATH);
		return -1;
	}

	err = ioctl(fd, FLUX_DEV_IO_UINTR_SETUP, handler);
	if (err < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to setup uintr per-cpu data\n");
		close(fd);
		return -1;
	}

	FLUX_LOG(FLUX_LOG_DEBUG, "uintr enabled\n");
	/* keep fd open while this thread is active */
	flux_uintr_fd = fd;
#endif

	return 0;
}

int flux_kmod_exit_percpu(void)
{
#ifdef CONFIG_FLUX_UINTR
	if (flux_uintr_fd >= 0) {
		if (close(flux_uintr_fd) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to close uintr percpu fd\n");
			return -1;
		}
		flux_uintr_fd = -1;
	}
#endif

	return 0;
}

int flux_kmod_init(void)
{
	int fd;
	void *shm_addr;

	fd = open(FLUX_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n", FLUX_DEV_PATH);
		return -1;
	}

	shm_addr = mmap(NULL, PGSIZE_2MB, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, 0);
	if (shm_addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mmap shared memory\n");
		close(fd);
		return -1;
	}

	flux_shm = (struct flux_shm *)shm_addr;
	WRITE_ONCE(flux_shm->used_cpus, flux_env.nr_cpus);

	close(fd);

	return 0;
}

int flux_kmod_disable_mmap_hooks(void)
{
	int fd;

	fd = open(FLUX_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n", FLUX_DEV_PATH);
		return -1;
	}

	if (ioctl(fd, FLUX_DEV_IO_END_MAP_SHARED) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to disable mmap hooks\n");
		close(fd);
		return -1;
	}

	ioctl(fd, FLUX_DEV_IO_DUMP_VMAS);

	close(fd);

	return 0;
}
