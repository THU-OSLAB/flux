#ifndef _FLUX_IOKD_CLIENT_INTERNAL_H
#define _FLUX_IOKD_CLIENT_INTERNAL_H

#include "../iokd.h"

int flux_iokd_socket_send(int fd, const void *buf, size_t len, int send_fd);
int flux_iokd_socket_send_fds(int fd, const void *buf, size_t len,
			      const int *send_fds, size_t nr_fds);
int flux_iokd_socket_recv(int fd, void *buf, size_t len, int *recv_fd);
void *flux_iokd_client_control(void *arg);

#endif /* _FLUX_IOKD_CLIENT_INTERNAL_H */
