#define _GNU_SOURCE

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <flux.h>

#include "iok_client_internal.h"

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif

int flux_iok_socket_send(int fd, const void *buf, size_t len, int send_fd)
{
	const char *pos = buf;
	size_t done = 0;

	while (done < len) {
		struct msghdr msg = { 0 };
		struct iovec iov = {
			.iov_base = (void *)(pos + done),
			.iov_len = len - done,
		};
		char cmsgbuf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr *cmsg;
		ssize_t ret;

		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		if (!done && send_fd >= 0) {
			memset(cmsgbuf, 0, sizeof(cmsgbuf));
			msg.msg_control = cmsgbuf;
			msg.msg_controllen = sizeof(cmsgbuf);
			cmsg = CMSG_FIRSTHDR(&msg);
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(int));
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

int flux_iok_socket_recv_fds(int fd, void *buf, size_t len, int *recv_fds,
			     size_t nr_fds)
{
	char *pos = buf;
	int fds[2] = { -1, -1 };
	int error = -FLUX_EPROTO;
	size_t done = 0;
	size_t received = 0;

	if (nr_fds > 2 || (nr_fds && !recv_fds))
		return -FLUX_EINVAL;

	for (size_t i = 0; i < nr_fds; i++)
		recv_fds[i] = -1;

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
			too_many = received > nr_fds ||
				   count > nr_fds - received;
			for (i = 0; i < count; i++) {
				if (received < nr_fds)
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
	if (received != nr_fds)
		goto fail_fds;
	if (nr_fds)
		memcpy(recv_fds, fds, nr_fds * sizeof(*recv_fds));

	return 0;

fail_fds:
	for (size_t i = 0; i < received; i++)
		close(fds[i]);
	return error;
}

int flux_iok_socket_recv(int fd, void *buf, size_t len, int *recv_fd)
{
	return flux_iok_socket_recv_fds(fd, buf, len, recv_fd, recv_fd ? 1 : 0);
}
