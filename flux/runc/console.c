#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <flux.h>

#include "runc.h"

void flux_runc_console_detach_stdio_if_needed(bool terminal)
{
	int nullfd;

	if (terminal)
		return;

	nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (nullfd < 0)
		_exit(127);

	if (dup2(nullfd, STDIN_FILENO) < 0 || dup2(nullfd, STDOUT_FILENO) < 0 ||
	    dup2(nullfd, STDERR_FILENO) < 0) {
		close(nullfd);
		_exit(127);
	}

	if (nullfd > STDERR_FILENO)
		close(nullfd);
}

int flux_runc_console_open_detached_pty(int *master_fd, int *slave_fd)
{
	char slave_name[PATH_MAX];
	int master;
	int slave;

	*master_fd = -1;
	*slave_fd = -1;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0)
		return -errno;

	if (grantpt(master) < 0 || unlockpt(master) < 0) {
		int ret = -errno;

		close(master);
		return ret;
	}

	if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
		int ret = -errno;

		close(master);
		return ret;
	}

	slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave < 0) {
		int ret = -errno;

		close(master);
		return ret;
	}
	{
		struct termios attr;

		if (tcgetattr(slave, &attr) < 0) {
			int ret = -errno;

			close(slave);
			close(master);
			return ret;
		}
		cfmakeraw(&attr);
		if (tcsetattr(slave, TCSANOW, &attr) < 0) {
			int ret = -errno;

			close(slave);
			close(master);
			return ret;
		}
	}

	*master_fd = master;
	*slave_fd = slave;
	return 0;
}

int flux_runc_console_set_size(int fd, unsigned int width,
			       unsigned int height)
{
	struct winsize size = {
		.ws_col = (unsigned short)width,
		.ws_row = (unsigned short)height,
	};

	if (fd < 0 || width > USHRT_MAX || height > USHRT_MAX)
		return -EINVAL;
	if (ioctl(fd, TIOCSWINSZ, &size) < 0)
		return -errno;

	return 0;
}

void flux_runc_console_setup_detached_terminal(int master_fd, int slave_fd)
{
	char slave_name[PATH_MAX];
	bool reopen_slave = false;
	int tty_fd = slave_fd;

	if (master_fd >= 0 &&
	    ptsname_r(master_fd, slave_name, sizeof(slave_name)) == 0) {
		reopen_slave = true;
		if (slave_fd >= 0) {
			close(slave_fd);
			tty_fd = -1;
		}
	}

	if (master_fd >= 0)
		close(master_fd);

	if (setsid() < 0)
		_exit(127);

	if (reopen_slave) {
		tty_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (tty_fd < 0)
			_exit(127);
	}

#ifdef TIOCSCTTY
	if (ioctl(tty_fd, TIOCSCTTY, 0) < 0)
		_exit(127);
#endif

	if (dup2(tty_fd, STDIN_FILENO) < 0 || dup2(tty_fd, STDOUT_FILENO) < 0 ||
	    dup2(tty_fd, STDERR_FILENO) < 0)
		_exit(127);

	if (tty_fd > STDERR_FILENO)
		close(tty_fd);
}

int flux_runc_console_send_fd(const char *socket_path, int fd)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	struct msghdr msg = { 0 };
	struct iovec iov;
	char control[CMSG_SPACE(sizeof(fd))];
	char data = '\0';
	int sock;
	int ret = 0;

	if (!socket_path || !socket_path[0] || fd < 0)
		return -EINVAL;
	if (strlen(socket_path) >= sizeof(addr.sun_path))
		return -ENAMETOOLONG;

	sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0)
		return -errno;

	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		goto out;
	}

	iov.iov_base = &data;
	iov.iov_len = sizeof(data);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	{
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
		msg.msg_controllen = cmsg->cmsg_len;
	}

	if (sendmsg(sock, &msg, 0) < 0)
		ret = -errno;

out:
	close(sock);
	return ret;
}
