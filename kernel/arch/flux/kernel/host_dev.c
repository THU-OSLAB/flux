#include <linux/fcntl.h>
#include <linux/printk.h>
#include <linux/syscalls.h>
#include <asm/ptrace.h>
#include <asm/syscalls.h>
#include <asm/host_dev.h>
#include <asm/unistd.h>
#include <asm/ioctl.h>

int flux_host_dev_fd = -1;

void flux_host_dev_init(void)
{
	flux_host_dev_fd =
		host_syscall(__NR_open, FLUX_MM_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (flux_host_dev_fd < 0) {
		pr_err("failed to open " FLUX_MM_DEV_PATH "\n");
	}
}

void flux_host_dev_exit(void)
{
	if (flux_host_dev_fd >= 0) {
		host_syscall(__NR_close, flux_host_dev_fd);
		flux_host_dev_fd = -1;
	}
}
