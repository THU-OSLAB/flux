#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <kernel/linux/errno.h>

#include "internal.h"

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif

int flux_iokd_socket_send_fds(int fd, const void *buf, size_t len,
			      const int *send_fds, size_t nr_fds)
{
	const char *pos = buf;
	size_t done = 0;

	if (nr_fds && (!send_fds || nr_fds > 2))
		return -FLUX_EINVAL;
	while (done < len) {
		struct msghdr msg = { 0 };
		struct iovec iov = {
			.iov_base = (void *)(pos + done),
			.iov_len = len - done,
		};
		char cmsgbuf[CMSG_SPACE(sizeof(int) * 2)];
		struct cmsghdr *cmsg;
		ssize_t ret;

		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		if (!done && nr_fds) {
			memset(cmsgbuf, 0, sizeof(cmsgbuf));
			msg.msg_control = cmsgbuf;
			msg.msg_controllen = CMSG_SPACE(sizeof(int) * nr_fds);
			cmsg = CMSG_FIRSTHDR(&msg);
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nr_fds);
			memcpy(CMSG_DATA(cmsg), send_fds, sizeof(int) * nr_fds);
		}

		ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
		if (ret < 0) {
			if (errno == FLUX_EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -FLUX_EIO;
		done += (size_t)ret;
	}

	return 0;
}

int flux_iokd_socket_send(int fd, const void *buf, size_t len, int send_fd)
{
	return flux_iokd_socket_send_fds(fd, buf, len,
					 send_fd >= 0 ? &send_fd : NULL,
					 send_fd >= 0 ? 1 : 0);
}

int flux_iokd_socket_recv(int fd, void *buf, size_t len, int *recv_fd)
{
	char *pos = buf;
	int fds[2] = { -1, -1 };
	int error = -FLUX_EPROTO;
	size_t done = 0;
	size_t max_fds = recv_fd ? 1 : 0;
	size_t received = 0;

	if (recv_fd)
		*recv_fd = -1;

	while (done < len) {
		struct msghdr msg = { 0 };
		struct iovec iov = {
			.iov_base = pos + done,
			.iov_len = len - done,
		};
		char cmsgbuf[CMSG_SPACE(sizeof(fds))];
		struct cmsghdr *cmsg;
		ssize_t ret;

		memset(cmsgbuf, 0, sizeof(cmsgbuf));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);
		ret = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
		if (ret < 0) {
			if (errno == FLUX_EINTR)
				continue;
			error = -errno;
			goto fail_fds;
		}
		if (!ret) {
			error = -FLUX_EIO;
			goto fail_fds;
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
		     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			size_t data_len;
			size_t count;
			size_t i;
			bool too_many;
			int *data;

			if (cmsg->cmsg_level != SOL_SOCKET ||
			    cmsg->cmsg_type != SCM_RIGHTS)
				continue;
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(int)) ||
			    cmsg->cmsg_len > CMSG_LEN(sizeof(fds)))
				goto fail_fds;
			data_len = cmsg->cmsg_len - CMSG_LEN(0);
			if (data_len % sizeof(int))
				goto fail_fds;
			count = data_len / sizeof(int);
			data = (int *)CMSG_DATA(cmsg);
			too_many = received > max_fds ||
				   count > max_fds - received;
			for (i = 0; i < count; i++) {
				if (received < max_fds)
					fds[received++] = data[i];
				else
					close(data[i]);
			}
			if (too_many)
				goto fail_fds;
		}
		if (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
			goto fail_fds;
		done += (size_t)ret;
	}
	if (recv_fd)
		*recv_fd = received ? fds[0] : -1;

	return 0;

fail_fds:
	for (size_t i = 0; i < received; i++)
		close(fds[i]);
	return error;
}
