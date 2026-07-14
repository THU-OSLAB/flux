/*
 * sock.c - Flux fast socket implemented as regular file descriptor
 */

#define pr_fmt(fmt) "<fnet> " KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/eventpoll.h>
#include <linux/anon_inodes.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/poll.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uio.h>
#include <uapi/asm/ioctls.h>

#include "net.h"

enum { SOCK_UNBOUND = 0, SOCK_BOUND, SOCK_LISTENING, SOCK_CONNECTED };

struct fast_socket {
	wait_queue_head_t wq;
	atomic_long_t poll_events;
	u8 state;
	int backlog;
	struct net_addr addr;
	union {
		tcp_conn_t *conn;
		tcp_accq_t *accq;
	};
};

static const struct file_operations flux_fast_socket_fops;

static inline int sockaddr_to_net_addr(struct sockaddr *sa, int addrlen,
				       struct net_addr *addr)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)sa;

	if (!sa || sa->sa_family != AF_INET ||
	    addrlen < sizeof(struct sockaddr_in))
		return -EINVAL;

	addr->ip = ntohl(sin->sin_addr.s_addr);
	addr->port = ntohs(sin->sin_port);
	return 0;
}

static inline int usockaddr_to_net_addr(struct sockaddr __user *usa,
					int addrlen, struct net_addr *addr)
{
	struct sockaddr_storage kaddr;
	int err;

	err = move_addr_to_kernel(usa, addrlen, &kaddr);
	if (err)
		return err;

	return sockaddr_to_net_addr((struct sockaddr *)&kaddr, addrlen, addr);
}

static inline void net_addr_to_sockaddr(struct net_addr *addr,
					struct sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons(addr->port);
	sin->sin_addr.s_addr = htonl(addr->ip);
}

/* copied from net/socket.c */
static int move_addr_to_user(struct sockaddr_storage *kaddr, int klen,
			     void __user *uaddr, int __user *ulen)
{
	int err;
	int len;

	BUG_ON(klen > sizeof(struct sockaddr_storage));
	err = get_user(len, ulen);
	if (err)
		return err;
	if (len > klen)
		len = klen;
	if (len < 0)
		return -EINVAL;
	if (len && copy_to_user(uaddr, kaddr, len))
		return -EFAULT;

	return __put_user(klen, ulen);
}

static inline int net_addr_to_usockaddr(struct net_addr *addr,
					struct sockaddr __user *usa,
					int __user *ulen)
{
	struct sockaddr_in kaddr;

	net_addr_to_sockaddr(addr, &kaddr);
	return move_addr_to_user((struct sockaddr_storage *)&kaddr,
				 sizeof(kaddr), usa, ulen);
}

static void fsock_poll_set(unsigned long data, unsigned int events)
{
	struct fast_socket *sock = (struct fast_socket *)data;
	unsigned long mask = (unsigned long)(__force __poll_t)events;
	unsigned long old;
	bool changed;

	old = atomic_long_fetch_or(mask, &sock->poll_events);
	changed = (mask & ~old) != 0;

	/*
	 * Listener readiness is level-triggered, but EPOLLEXCLUSIVE wakes only
	 * one waiter. Allow the accept path to notify another waiter while the
	 * queue remains readable.
	 */
	if (changed || (sock->state == SOCK_LISTENING && (mask & POLLIN))) {
#ifdef CONFIG_FLUX_FAST_NET_WAKEUP
		__wake_up_on_current_cpu(&sock->wq, TASK_INTERRUPTIBLE,
					 poll_to_key(events));
#else
		wake_up_interruptible_poll(&sock->wq, events);
#endif
	}
}

static void fsock_poll_clear(unsigned long data, unsigned int events)
{
	struct fast_socket *sock = (struct fast_socket *)data;
	unsigned long mask = (unsigned long)(__force __poll_t)events;

	atomic_long_fetch_andnot(mask, &sock->poll_events);
}

static void fsock_poll_setup(struct fast_socket *sock)
{
	if (sock->state == SOCK_LISTENING) {
		flux_tcp_accq_poll_install_cb(sock->accq, fsock_poll_set,
					      fsock_poll_clear,
					      (unsigned long)sock);
	} else if (sock->state == SOCK_CONNECTED) {
		flux_tcp_poll_install_cb(sock->conn, fsock_poll_set,
					 fsock_poll_clear, (unsigned long)sock);
	}
}

static struct fast_socket *fsock_lookup(int fd, struct fd *f)
{
	struct fast_socket *sock;

	*f = fdget(fd);
	if (!f->file)
		return NULL;
	if (f->file->f_op != &flux_fast_socket_fops) {
		fdput(*f);
		return NULL;
	}
	sock = f->file->private_data;
	if (!sock) {
		fdput(*f);
		return NULL;
	}
	return sock;
}

static bool is_fsock_fd(int fd)
{
	struct fd f = fdget(fd);
	bool yes;

	if (!f.file)
		return false;
	yes = f.file->f_op == &flux_fast_socket_fops;
	fdput(f);
	return yes;
}

static long fsock_read_file(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct fast_socket *sock = file->private_data;

	if (!sock || sock->state != SOCK_CONNECTED)
		return -ENOTCONN;

	return flux_tcp_read2(sock->conn, buf, count, false,
			      (file->f_flags & O_NONBLOCK) != 0);
}

static long fsock_write_file(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct fast_socket *sock = file->private_data;

	if (!sock || sock->state != SOCK_CONNECTED)
		return -ENOTCONN;

	return flux_tcp_write2(sock->conn, buf, count,
			       (file->f_flags & O_NONBLOCK) != 0);
}

static int fsock_iov_from_iter(const struct iov_iter *iter, struct iovec **iovp,
			       int *iovcnt, bool nowait,
			       struct iovec stack_iov[UIO_FASTIOV])
{
	const struct iovec *src_iov;
	struct iovec *out_iov;
	size_t remaining;
	unsigned long nr_segs;
	unsigned long i, n = 0;

	if (!iter_is_iovec(iter) && !iter_is_ubuf(iter) &&
	    !iov_iter_is_kvec(iter))
		return -EOPNOTSUPP;

	remaining = iov_iter_count(iter);
	if (!remaining) {
		*iovp = NULL;
		*iovcnt = 0;
		return 0;
	}

	nr_segs = iter->nr_segs;
	if (!nr_segs || nr_segs > INT_MAX)
		return -EINVAL;

	if (nr_segs <= UIO_FASTIOV) {
		out_iov = stack_iov;
	} else {
		out_iov = kmalloc_array(nr_segs, sizeof(*out_iov),
					nowait ? GFP_NOWAIT : GFP_KERNEL);
		if (!out_iov)
			return -ENOMEM;
	}

	src_iov = iter_iov(iter);
	for (i = 0; i < nr_segs && remaining; i++) {
		void *base = src_iov[i].iov_base;
		size_t len = src_iov[i].iov_len;

		if (i == 0) {
			if (iter->iov_offset > len)
				goto invalid;
			base = (char *)base + iter->iov_offset;
			len -= iter->iov_offset;
		}

		if (!len)
			continue;

		if (len > remaining)
			len = remaining;

		out_iov[n].iov_base = base;
		out_iov[n].iov_len = len;
		n++;
		remaining -= len;
	}

	if (remaining)
		goto invalid;
	if (!n)
		goto invalid;

	*iovp = out_iov;
	*iovcnt = n;
	return 0;

invalid:
	if (out_iov != stack_iov)
		kfree(out_iov);
	return -EINVAL;
}

static ssize_t fsock_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct fast_socket *sock = file->private_data;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov;
	int iovcnt;
	ssize_t ret;
	int err;
	bool nowait;
	bool nonblocking;

	if (!sock || sock->state != SOCK_CONNECTED)
		return -ENOTCONN;

	nowait = (iocb->ki_flags & IOCB_NOWAIT) != 0;
	nonblocking = (file->f_flags & O_NONBLOCK) != 0 || nowait;

	err = fsock_iov_from_iter(to, &iov, &iovcnt, nowait, iovstack);
	if (err)
		return err;
	if (!iovcnt)
		return 0;

	ret = flux_tcp_readv2(sock->conn, iov, iovcnt, false, nonblocking);
	if (iov != iovstack)
		kfree(iov);
	if (ret > 0) {
		iov_iter_advance(to, ret);
		iocb->ki_pos += ret;
	}

	return ret;
}

static ssize_t fsock_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct fast_socket *sock = file->private_data;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov;
	int iovcnt;
	ssize_t ret;
	int err;
	bool nowait;
	bool nonblocking;

	if (!sock || sock->state != SOCK_CONNECTED)
		return -ENOTCONN;

	nowait = (iocb->ki_flags & IOCB_NOWAIT) != 0;
	nonblocking = (file->f_flags & O_NONBLOCK) != 0 || nowait;

	err = fsock_iov_from_iter(from, &iov, &iovcnt, nowait, iovstack);
	if (err)
		return err;
	if (!iovcnt)
		return 0;

	ret = flux_tcp_writev2(sock->conn, iov, iovcnt, nonblocking);
	if (iov != iovstack)
		kfree(iov);
	if (ret > 0) {
		iov_iter_advance(from, ret);
		iocb->ki_pos += ret;
	}

	return ret;
}

static __poll_t fsock_poll_file(struct file *file, poll_table *wait)
{
	struct fast_socket *sock = file->private_data;
	__poll_t mask;

	if (!sock)
		return EPOLLERR;

	poll_wait(file, &sock->wq, wait);

	mask = (__force __poll_t)atomic_long_read_acquire(&sock->poll_events);

	return mask;
}

static int fsock_release(struct inode *inode, struct file *file)
{
	struct fast_socket *sock = file->private_data;

	if (!sock)
		return 0;

	if (sock->state == SOCK_CONNECTED)
		flux_tcp_close(sock->conn);
	else if (sock->state == SOCK_LISTENING)
		flux_tcp_accq_close(sock->accq);

	kfree(sock);
	file->private_data = NULL;
	return 0;
}

static long fsock_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fast_socket *sock = file->private_data;
	uint32_t bytes = 0;
	int __user *argp = (int __user *)arg;

	if (!sock)
		return -EINVAL;

	switch (cmd) {
	case FIONREAD:
		if (!argp)
			return -EINVAL;
		if (sock->state == SOCK_CONNECTED)
			bytes = flux_tcp_get_input_bytes(sock->conn);
		if (bytes > 0x7fffffffU)
			bytes = 0x7fffffffU;
		return put_user((int)bytes, argp);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations flux_fast_socket_fops = {
	.owner = THIS_MODULE,
	.read = fsock_read_file,
	.write = fsock_write_file,
	.read_iter = fsock_read_iter,
	.write_iter = fsock_write_iter,
	.poll = fsock_poll_file,
	.unlocked_ioctl = fsock_ioctl,
	.compat_ioctl = fsock_ioctl,
	.release = fsock_release,
	.llseek = no_llseek,
};

static long __sys_fast_socket(int family, int type, int protocol)
{
	struct fast_socket *sock;
	int flags;
	int fd_flags, fd;

	if (family != AF_INET || (type & SOCK_TYPE_MASK) != SOCK_STREAM)
		return -EAFNOSUPPORT;
	if (protocol && protocol != IPPROTO_TCP)
		return -EPROTONOSUPPORT;

	flags = type & ~SOCK_TYPE_MASK;
	if (SOCK_NONBLOCK != O_NONBLOCK && (flags & SOCK_NONBLOCK))
		flags = (flags & ~SOCK_NONBLOCK) | O_NONBLOCK;

	fd_flags = O_RDWR | (flags & (O_NONBLOCK | O_CLOEXEC));

	sock = kzalloc(sizeof(*sock), GFP_KERNEL);
	if (!sock)
		return -ENOMEM;

	init_waitqueue_head(&sock->wq);
	sock->state = SOCK_UNBOUND;

	fd = anon_inode_getfd("[flux-fast-socket]", &flux_fast_socket_fops,
			      sock, fd_flags);
	if (fd < 0) {
		kfree(sock);
		return fd;
	}

	return fd;
}

static long __sys_fast_bind(int fd, struct sockaddr __user *umyaddr,
			    int addrlen)
{
	struct fast_socket *sock;
	struct fd f;
	int err;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;

	err = 0;
	if (sock->state != SOCK_UNBOUND)
		err = -EINVAL;
	else
		err = usockaddr_to_net_addr(umyaddr, addrlen, &sock->addr);

	fdput(f);
	return err;
}

static long __sys_fast_listen(int fd, int backlog)
{
	struct fast_socket *sock;
	struct fd f;
	int err;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;

	if (sock->state == SOCK_LISTENING) {
		sock->backlog = backlog;
		fdput(f);
		return 0;
	}

	err = flux_tcp_listen(sock->addr, backlog, &sock->accq);
	if (!err) {
		sock->state = SOCK_LISTENING;
		sock->backlog = backlog;
		fsock_poll_setup(sock);
	}

	fdput(f);
	return err;
}

static int fsock_create_from_conn(tcp_conn_t *conn, int flags)
{
	struct fast_socket *sock;
	int fd;

	sock = kzalloc(sizeof(*sock), GFP_KERNEL);
	if (!sock)
		return -ENOMEM;

	init_waitqueue_head(&sock->wq);
	sock->conn = conn;
	sock->state = SOCK_CONNECTED;
	fsock_poll_setup(sock);

	fd = anon_inode_getfd("[flux-fast-socket]", &flux_fast_socket_fops,
			      sock,
			      O_RDWR | (flags & (O_NONBLOCK | O_CLOEXEC)));
	if (fd < 0) {
		flux_tcp_close(conn);
		kfree(sock);
		return fd;
	}

	return fd;
}

static long __sys_fast_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
			       int __user *upeer_addrlen, int flags)
{
	struct fast_socket *sock;
	struct fd f;
	tcp_conn_t *conn;
	struct net_addr raddr;
	int err, child_fd;

	if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
		return -EINVAL;
	if (SOCK_NONBLOCK != O_NONBLOCK && (flags & SOCK_NONBLOCK))
		flags = (flags & ~SOCK_NONBLOCK) | O_NONBLOCK;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;
	if (sock->state != SOCK_LISTENING) {
		fdput(f);
		return -EINVAL;
	}

	err = flux_tcp_accept(sock->accq,
			      (f.file->f_flags & O_NONBLOCK) != 0, &conn);
	if (err) {
		fdput(f);
		return err;
	}
	child_fd = fsock_create_from_conn(conn, flags);
	if (child_fd < 0) {
		fdput(f);
		return child_fd;
	}

	if (upeer_sockaddr) {
		raddr = flux_tcp_remote_addr(conn);
		err = net_addr_to_usockaddr(&raddr, upeer_sockaddr,
					    upeer_addrlen);
		if (err) {
			sys_close(child_fd);
			fdput(f);
			return err;
		}
	}

	fdput(f);
	return child_fd;
}

static long __sys_fast_shutdown(int fd, int how)
{
	struct fast_socket *sock;
	struct fd f;
	long err = 0;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;

	if (sock->state == SOCK_CONNECTED)
		err = flux_tcp_shutdown(sock->conn, how);
	else if (sock->state == SOCK_LISTENING)
		flux_tcp_accq_shutdown(sock->accq);
	else
		err = -ENOTCONN;

	fdput(f);
	return err;
}

static long __sys_fast_connect(int fd, struct sockaddr __user *uservaddr,
			       int addrlen)
{
	struct fast_socket *sock;
	struct fd f;
	struct net_addr raddr;
	bool nonblocking;
	int err = 0;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;

	nonblocking = (f.file->f_flags & O_NONBLOCK) != 0;

	if (sock->state == SOCK_CONNECTED) {
		err = nonblocking ? flux_tcp_get_status(sock->conn) : -EISCONN;
		goto out;
	}
	if (sock->state == SOCK_LISTENING) {
		err = -EINVAL;
		goto out;
	}

	err = usockaddr_to_net_addr(uservaddr, addrlen, &raddr);
	if (err)
		goto out;

	if (nonblocking)
		err = flux_tcp_dial_nonblocking(sock->addr, raddr, &sock->conn);
	else
		err = flux_tcp_dial(sock->addr, raddr, &sock->conn);
	if (err)
		goto out;

	sock->state = SOCK_CONNECTED;
	fsock_poll_setup(sock);
	if (nonblocking)
		err = flux_tcp_get_status(sock->conn);
out:
	fdput(f);
	return err;
}

static long __sys_fast_sendto(int fd, void __user *buff, size_t len,
			      unsigned int flags, struct sockaddr __user *addr,
			      int addr_len)
{
	struct fast_socket *sock;
	struct fd f;
	bool nonblocking;
	ssize_t ret;

	nonblocking = (flags & MSG_DONTWAIT) != 0;
	flags &= ~(MSG_NOSIGNAL | MSG_DONTWAIT);
	if (unlikely(flags))
		return -EINVAL;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;
	if (sock->state != SOCK_CONNECTED) {
		fdput(f);
		return -ENOTCONN;
	}
	if (unlikely(addr)) {
		fdput(f);
		return -EISCONN;
	}

	nonblocking |= (f.file->f_flags & O_NONBLOCK) != 0;
	ret = flux_tcp_write2(sock->conn, buff, len, nonblocking);
	fdput(f);
	return ret;
}

static long __sys_fast_recvfrom(int fd, void __user *ubuf, size_t size,
				unsigned int flags,
				struct sockaddr __user *addr,
				int __user *addr_len)
{
	struct fast_socket *sock;
	struct fd f;
	bool peek;
	bool nonblocking;
	struct net_addr raddr;
	ssize_t err = 0;

	peek = (flags & MSG_PEEK) != 0;
	nonblocking = (flags & MSG_DONTWAIT) != 0;
	flags &= ~(MSG_NOSIGNAL | MSG_PEEK | MSG_DONTWAIT);
	if (unlikely(flags))
		return -EINVAL;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;
	if (sock->state != SOCK_CONNECTED) {
		fdput(f);
		return -ENOTCONN;
	}

	if (addr) {
		raddr = flux_tcp_remote_addr(sock->conn);
		err = net_addr_to_usockaddr(&raddr, addr, addr_len);
		if (err) {
			fdput(f);
			return err;
		}
	}

	nonblocking |= (f.file->f_flags & O_NONBLOCK) != 0;
	err = flux_tcp_read2(sock->conn, ubuf, size, peek, nonblocking);
	fdput(f);
	return err;
}

static long __sys_fast_sendmsg(int fd, struct user_msghdr __user *umsg,
			       unsigned int flags)
{
	struct fast_socket *sock;
	struct fd f;
	struct user_msghdr msg;
	bool nonblocking;
	ssize_t ret;

	nonblocking = (flags & MSG_DONTWAIT) != 0;
	flags &= ~(MSG_NOSIGNAL | MSG_DONTWAIT);
	if (unlikely(flags))
		return -EINVAL;

	if (copy_from_user(&msg, umsg, sizeof(msg)))
		return -EFAULT;
	if (msg.msg_name || msg.msg_namelen)
		return -EISCONN;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;
	if (sock->state != SOCK_CONNECTED) {
		fdput(f);
		return -ENOTCONN;
	}

	nonblocking |= (f.file->f_flags & O_NONBLOCK) != 0;
	ret = flux_tcp_writev2(sock->conn, msg.msg_iov, msg.msg_iovlen,
			       nonblocking);
	fdput(f);
	return ret;
}

static long __sys_fast_recvmsg(int fd, struct user_msghdr __user *umsg,
			       unsigned int flags)
{
	struct fast_socket *sock;
	struct fd f;
	struct user_msghdr msg;
	bool peek;
	bool nonblocking;
	struct net_addr raddr;
	ssize_t err = 0;

	peek = (flags & MSG_PEEK) != 0;
	nonblocking = (flags & MSG_DONTWAIT) != 0;
	flags &= ~(MSG_NOSIGNAL | MSG_PEEK | MSG_DONTWAIT);
	if (unlikely(flags))
		return -EINVAL;

	if (copy_from_user(&msg, umsg, sizeof(msg)))
		return -EFAULT;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;
	if (sock->state != SOCK_CONNECTED) {
		fdput(f);
		return -ENOTCONN;
	}

	if (msg.msg_name) {
		raddr = flux_tcp_remote_addr(sock->conn);
		err = net_addr_to_usockaddr(&raddr, msg.msg_name,
					    &msg.msg_namelen);
		if (err) {
			fdput(f);
			return err;
		}
	}

	nonblocking |= (f.file->f_flags & O_NONBLOCK) != 0;
	err = flux_tcp_readv2(sock->conn, msg.msg_iov, msg.msg_iovlen, peek,
			      nonblocking);
	fdput(f);
	return err;
}

static long __sys_fast_getsockopt(int fd, int level, int optname,
				  char __user *optval, int __user *optlen)
{
	struct fast_socket *sock;
	struct fd f;
	int val;
	int len;

	if (unlikely(level != SOL_SOCKET))
		return -EINVAL;
	if (get_user(len, optlen))
		return -EFAULT;
	if (len < sizeof(int))
		return -EINVAL;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;

	switch (optname) {
	case SO_ACCEPTCONN:
		val = (sock->state == SOCK_LISTENING) ? 1 : 0;
		break;
	case SO_DOMAIN:
		val = AF_INET;
		break;
	case SO_PROTOCOL:
		val = IPPROTO_TCP;
		break;
	case SO_TYPE:
		val = SOCK_STREAM;
		break;
	case SO_ERROR:
		val = (sock->state == SOCK_CONNECTED) ?
			      flux_tcp_get_status(sock->conn) :
			      0;
		break;
	default:
		fdput(f);
		return -EINVAL;
	}

	fdput(f);
	if (copy_to_user(optval, &val, sizeof(val)))
		return -EFAULT;
	if (put_user(sizeof(val), optlen))
		return -EFAULT;
	return 0;
}

static long __sys_fast_setsockopt(int fd, int level, int optname,
				  char __user *user_optval, int optlen)
{
	if (!is_fsock_fd(fd))
		return -ENOTSOCK;

	pr_debug("setsockopt not implemented for fast sockets\n");
	return 0;
}

static long __sys_fast_getpeername(int fd, struct sockaddr __user *usockaddr,
				   int __user *uaddrlen)
{
	struct fast_socket *sock;
	struct fd f;
	struct net_addr raddr;
	int err;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;
	if (sock->state != SOCK_CONNECTED) {
		fdput(f);
		return -ENOTCONN;
	}

	raddr = flux_tcp_remote_addr(sock->conn);
	fdput(f);
	err = net_addr_to_usockaddr(&raddr, usockaddr, uaddrlen);
	return err;
}

static long __sys_fast_getsockname(int fd, struct sockaddr __user *usockaddr,
				   int __user *uaddrlen)
{
	struct fast_socket *sock;
	struct fd f;
	struct net_addr laddr;
	int err;

	sock = fsock_lookup(fd, &f);
	if (!sock)
		return -ENOTSOCK;

	if (sock->state == SOCK_CONNECTED)
		laddr = flux_tcp_local_addr(sock->conn);
	else
		laddr = sock->addr;

	fdput(f);
	err = net_addr_to_usockaddr(&laddr, usockaddr, uaddrlen);
	return err;
}

SYSCALL_DEFINE3(fast_socket, int, family, int, type, int, protocol)
{
	if (family == AF_INET && (type & SOCK_TYPE_MASK) == SOCK_STREAM) {
		if (!protocol)
			protocol = IPPROTO_TCP;
		return __sys_fast_socket(family, type, protocol);
	}

	return __sys_socket(family, type, protocol);
}

SYSCALL_DEFINE3(fast_bind, int, fd, struct sockaddr __user *, umyaddr, int,
		addrlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_bind(fd, umyaddr, addrlen);

	return __sys_bind(fd, umyaddr, addrlen);
}

SYSCALL_DEFINE2(fast_listen, int, fd, int, backlog)
{
	if (is_fsock_fd(fd))
		return __sys_fast_listen(fd, backlog);

	return __sys_listen(fd, backlog);
}

SYSCALL_DEFINE4(fast_accept4, int, fd, struct sockaddr __user *, upeer_sockaddr,
		int __user *, upeer_addrlen, int, flags)
{
	if (is_fsock_fd(fd))
		return __sys_fast_accept4(fd, upeer_sockaddr, upeer_addrlen,
					  flags);

	return __sys_accept4(fd, upeer_sockaddr, upeer_addrlen, flags);
}

SYSCALL_DEFINE3(fast_accept, int, fd, struct sockaddr __user *, upeer_sockaddr,
		int __user *, upeer_addrlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_accept4(fd, upeer_sockaddr, upeer_addrlen, 0);

	return __sys_accept4(fd, upeer_sockaddr, upeer_addrlen, 0);
}

SYSCALL_DEFINE2(fast_shutdown, int, fd, int, how)
{
	if (is_fsock_fd(fd))
		return __sys_fast_shutdown(fd, how);

	return __sys_shutdown(fd, how);
}

SYSCALL_DEFINE3(fast_connect, int, fd, struct sockaddr __user *, uservaddr, int,
		addrlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_connect(fd, uservaddr, addrlen);

	return __sys_connect(fd, uservaddr, addrlen);
}

SYSCALL_DEFINE6(fast_sendto, int, fd, void __user *, buff, size_t, len,
		unsigned int, flags, struct sockaddr __user *, addr, int,
		addr_len)
{
	if (is_fsock_fd(fd))
		return __sys_fast_sendto(fd, buff, len, flags, addr, addr_len);

	return __sys_sendto(fd, buff, len, flags, addr, addr_len);
}

SYSCALL_DEFINE6(fast_recvfrom, int, fd, void __user *, ubuf, size_t, size,
		unsigned int, flags, struct sockaddr __user *, addr,
		int __user *, addr_len)
{
	if (is_fsock_fd(fd))
		return __sys_fast_recvfrom(fd, ubuf, size, flags, addr,
					   addr_len);

	return __sys_recvfrom(fd, ubuf, size, flags, addr, addr_len);
}

SYSCALL_DEFINE3(fast_sendmsg, int, fd, struct user_msghdr __user *, umsg,
		unsigned int, flags)
{
	if (is_fsock_fd(fd))
		return __sys_fast_sendmsg(fd, umsg, flags);

	return __sys_sendmsg(fd, umsg, flags, true);
}

SYSCALL_DEFINE3(fast_recvmsg, int, fd, struct user_msghdr __user *, umsg,
		unsigned int, flags)
{
	if (is_fsock_fd(fd))
		return __sys_fast_recvmsg(fd, umsg, flags);

	return __sys_recvmsg(fd, umsg, flags, true);
}

SYSCALL_DEFINE5(fast_getsockopt, int, fd, int, level, int, optname,
		char __user *, optval, int __user *, optlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_getsockopt(fd, level, optname, optval,
					     optlen);

	return __sys_getsockopt(fd, level, optname, optval, optlen);
}

SYSCALL_DEFINE5(fast_setsockopt, int, fd, int, level, int, optname,
		char __user *, user_optval, int, optlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_setsockopt(fd, level, optname, user_optval,
					     optlen);

	return __sys_setsockopt(fd, level, optname, user_optval, optlen);
}

SYSCALL_DEFINE3(fast_getpeername, int, fd, struct sockaddr __user *, usockaddr,
		int __user *, uaddrlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_getpeername(fd, usockaddr, uaddrlen);

	return __sys_getpeername(fd, usockaddr, uaddrlen);
}

SYSCALL_DEFINE3(fast_getsockname, int, fd, struct sockaddr __user *, usockaddr,
		int __user *, uaddrlen)
{
	if (is_fsock_fd(fd))
		return __sys_fast_getsockname(fd, usockaddr, uaddrlen);

	return __sys_getsockname(fd, usockaddr, uaddrlen);
}
