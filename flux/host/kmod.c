#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <flux.h>

#include "kmod.h"

struct flux_shm *flux_shm;

static __thread int flux_uintr_fd = -1;

int flux_kmod_init_percpu(void *handler, void *synthetic_handler,
			  int logical_cpu,
			  unsigned long host_fsbase, void *signal_stack,
			  size_t signal_stack_slot_size)
{
	int err, fd;
	struct flux_uintr_setup setup = {
		.handler = (uintptr_t)handler,
		.synthetic_handler = (uintptr_t)synthetic_handler,
		.host_fsbase = host_fsbase,
		.signal_stack = (uintptr_t)signal_stack,
		.signal_stack_slot_size = signal_stack_slot_size,
		.logical_cpu = logical_cpu,
	};

	if (!signal_stack || !signal_stack_slot_size)
		return -1;
	if (flux_uintr_fd >= 0)
		return 0;

	fd = open(FLUX_UINTR_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n",
			 FLUX_UINTR_DEV_PATH);
		return -1;
	}

	err = ioctl(fd, FLUX_DEV_IO_UINTR_SETUP, &setup);
	if (err < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to setup uintr per-cpu data\n");
		close(fd);
		return -1;
	}

	FLUX_LOG(FLUX_LOG_DEBUG, "uintr enabled\n");
	/* keep fd open while this thread is active */
	flux_uintr_fd = fd;

	return 0;
}

int flux_kmod_exit_percpu(void)
{
	if (flux_uintr_fd >= 0) {
		if (close(flux_uintr_fd) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to close uintr percpu fd\n");
			return -1;
		}
		flux_uintr_fd = -1;
	}

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

	close(fd);

	return 0;
}
