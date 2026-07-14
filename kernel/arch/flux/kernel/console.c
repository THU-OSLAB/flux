// SPDX-License-Identifier: GPL-2.0
/*
 * Flux console driver
 *
 * This provides a printk console plus session-backed stdio/tty surfaces.
 */

#define pr_fmt(fmt) "flux_cons: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/xarray.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "../../../drivers/tty/tty.h"
#include <asm/host_ops.h>
#include <asm/console.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>

#define FLUX_RUNC_STATE_ROOT "/run/flux-runc"
#define FLUX_RUNC_SESSION_DIR "sessions"
#define FLUX_RUNC_USER_STATE_ROOT_FMT "/run/user/%lu/flux-runc"
#define FLUX_CONSOLE_INPUT_BUF_SIZE 4096
#define FLUX_CONSOLE_EOF_SLEEP_MS 100
#define FLUX_CONS_TTY_MAX_SESSIONS 256
#define FLUX_CONS_TTY_NAME "fluxcons"

struct flux_cons_backend {
	long stdin_fd;
	long stdout_fd;
	long stderr_fd;
};

static const struct flux_cons flux_cons_table[] = {
	{
		.id = FLUX_EXEC_SESSION_NONE,
		.kind = FLUX_CONS_KIND_NONE,
	},
};
static const struct flux_cons_backend flux_cons_backend0 = {
	.stdin_fd = 0,
	.stdout_fd = 1,
	.stderr_fd = 2,
};
static DEFINE_XARRAY(flux_cons_tty_by_id);
static DEFINE_XARRAY(flux_cons_tty_by_line);
static DEFINE_IDA(flux_cons_tty_lines);
static DEFINE_MUTEX(flux_cons_tty_lock);
static struct tty_driver *flux_cons_tty_driver;
static struct file_operations flux_cons_tty_file_fops;

struct flux_cons_input_buf;
struct flux_cons_tty_session;

struct flux_cons_file_ctx {
	long host_fd;
	bool close_on_release;
	struct flux_cons_input_buf *input;
};

struct flux_cons_input_buf {
	long host_fd;
	struct task_struct *thread;
	spinlock_t lock;
	wait_queue_head_t read_waitq;
	wait_queue_head_t space_waitq;
	u8 buf[FLUX_CONSOLE_INPUT_BUF_SIZE];
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	bool eof;
};

struct flux_cons_tty_session {
	u32 session_id;
	unsigned int line;
	struct tty_port port;
	struct task_struct *input_thread;
	long stdin_fd;
	long stdout_fd;
	long stderr_fd;
	bool persistent;
};

static struct flux_cons_tty_session flux_cons_tty0 = {
	.session_id = FLUX_EXEC_SESSION_NONE,
	.line = 0,
	.stdin_fd = 0,
	.stdout_fd = 1,
	.stderr_fd = 2,
	.persistent = true,
};

static int flux_cons_install_files(struct file *files[3], u32 session_id);
#ifdef CONFIG_FLUX_RUNC
static int flux_cons_host_open_file(u32 session_id, const char *name, int flags);
#endif

static size_t flux_cons_input_copy_to_user(struct flux_cons_input_buf *input,
					   char __user *buf, size_t len)
{
	size_t copied = 0;

	while (copied < len) {
		size_t chunk;
		unsigned int tail;

		spin_lock(&input->lock);
		if (!input->count) {
			spin_unlock(&input->lock);
			break;
		}

		tail = input->tail;
		chunk = min_t(size_t, len - copied, input->count);
		chunk = min_t(size_t, chunk,
			      FLUX_CONSOLE_INPUT_BUF_SIZE - tail);
		spin_unlock(&input->lock);

		if (copy_to_user(buf + copied, input->buf + tail, chunk))
			break;

		spin_lock(&input->lock);
		input->tail =
			(input->tail + chunk) % FLUX_CONSOLE_INPUT_BUF_SIZE;
		input->count -= (unsigned int)chunk;
		spin_unlock(&input->lock);
		wake_up_interruptible(&input->space_waitq);
		copied += chunk;
	}

	return copied;
}

static int flux_cons_input_thread_fn(void *arg)
{
	struct flux_cons_input_buf *input = arg;
	u8 buf[256];

	while (!kthread_should_stop()) {
		long ret;
		struct pollfd pfd = {
			.fd = (int)input->host_fd,
			.events = POLLIN,
		};

		ret = host_syscall(__NR_poll, &pfd, 1, 0);
		if (ret <= 0 || !(pfd.revents & POLLIN)) {
			schedule_timeout_interruptible(
				msecs_to_jiffies(FLUX_CONSOLE_EOF_SLEEP_MS));
			continue;
		}

		ret = host_syscall(__NR_read, input->host_fd, buf, sizeof(buf));
		if (ret > 0) {
			size_t off = 0;

			while (off < (size_t)ret) {
				size_t chunk;

				wait_event_interruptible(
					input->space_waitq,
					kthread_should_stop() ||
						input->count <
							FLUX_CONSOLE_INPUT_BUF_SIZE);
				if (kthread_should_stop())
					break;

				spin_lock(&input->lock);
				chunk = min_t(size_t, (size_t)ret - off,
					      FLUX_CONSOLE_INPUT_BUF_SIZE -
						      input->count);
				chunk = min_t(size_t, chunk,
					      FLUX_CONSOLE_INPUT_BUF_SIZE -
						      input->head);
				memcpy(input->buf + input->head, buf + off,
				       chunk);
				input->head = (input->head + chunk) %
					      FLUX_CONSOLE_INPUT_BUF_SIZE;
				input->count += (unsigned int)chunk;
				spin_unlock(&input->lock);
				wake_up_interruptible(&input->read_waitq);
				off += chunk;
			}
			continue;
		}

		if (ret == 0) {
			spin_lock(&input->lock);
			input->eof = true;
			spin_unlock(&input->lock);
			wake_up_interruptible(&input->read_waitq);
		}

		schedule_timeout_interruptible(
			msecs_to_jiffies(FLUX_CONSOLE_EOF_SLEEP_MS));
	}

	return 0;
}

static struct flux_cons_input_buf *flux_cons_input_buf_create(long host_fd)
{
	struct flux_cons_input_buf *input;

	input = kzalloc(sizeof(*input), GFP_KERNEL);
	if (!input)
		return ERR_PTR(-ENOMEM);

	input->host_fd = host_fd;
	spin_lock_init(&input->lock);
	init_waitqueue_head(&input->read_waitq);
	init_waitqueue_head(&input->space_waitq);
	input->thread =
		kthread_run(flux_cons_input_thread_fn, input, "flux_cons_in");
	if (IS_ERR(input->thread)) {
		long ret = PTR_ERR(input->thread);

		kfree(input);
		return ERR_PTR(ret);
	}

	return input;
}

static ssize_t flux_cons_file_read(struct file *file, char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct flux_cons_file_ctx *file_ctx = file->private_data;
	struct flux_cons_input_buf *input;
	size_t copied;
	int ret;

	if (!file_ctx || file_ctx->host_fd < 0)
		return -EBADF;

	input = file_ctx->input;
	if (input) {
		if (len == 0)
			return 0;

		ret = wait_event_interruptible(input->read_waitq,
					       input->count > 0 || input->eof);
		if (ret < 0)
			return ret;

		copied = flux_cons_input_copy_to_user(input, buf, len);
		if (copied > 0)
			return (ssize_t)copied;
		if (input->eof)
			return 0;
		return -EFAULT;
	}

	return host_syscall(__NR_read, file_ctx->host_fd, buf, len);
}

static ssize_t flux_cons_file_write(struct file *file, const char __user *buf,
				    size_t len, loff_t *ppos)
{
	struct flux_cons_file_ctx *file_ctx = file->private_data;

	if (!file_ctx || file_ctx->host_fd < 0)
		return -EBADF;

	return host_syscall(__NR_write, file_ctx->host_fd, buf, len);
}

static void flux_cons_write_host(long host_fd, const char *buf, size_t len)
{
	size_t off = 0;

	if (host_fd < 0 || !buf)
		return;

	while (off < len) {
		long ret =
			host_syscall(__NR_write, host_fd, buf + off, len - off);

		if (ret <= 0)
			break;
		off += (size_t)ret;
	}
}

static struct flux_cons_tty_session *
flux_cons_tty_session_from_port(struct tty_port *port)
{
	return container_of(port, struct flux_cons_tty_session, port);
}

static void flux_cons_tty_session_stop_input(struct flux_cons_tty_session *sess)
{
	if (!sess->input_thread)
		return;

	kthread_stop(sess->input_thread);
	sess->input_thread = NULL;
}

static int flux_cons_tty_input_thread_fn(void *arg)
{
	struct flux_cons_tty_session *sess = arg;
	u8 buf[256];

	while (!kthread_should_stop()) {
		struct pollfd pfd = {
			.fd = (int)sess->stdin_fd,
			.events = POLLIN | POLLHUP | POLLERR,
		};
		long ret;

		ret = host_syscall(__NR_poll, &pfd, 1, 0);
		if (ret <= 0) {
			schedule_timeout_interruptible(
				msecs_to_jiffies(FLUX_CONSOLE_EOF_SLEEP_MS));
			continue;
		}

		if (pfd.revents & (POLLHUP | POLLERR)) {
			tty_port_tty_hangup(&sess->port, false);
			break;
		}

		if (!(pfd.revents & POLLIN))
			continue;

		ret = host_syscall(__NR_read, sess->stdin_fd, buf, sizeof(buf));
		if (ret > 0) {
			tty_insert_flip_string(&sess->port, buf, (size_t)ret);
			tty_flip_buffer_push(&sess->port);
			continue;
		}

		if (ret == 0)
			tty_port_tty_hangup(&sess->port, false);

		schedule_timeout_interruptible(
			msecs_to_jiffies(FLUX_CONSOLE_EOF_SLEEP_MS));
	}

	return 0;
}

static int flux_cons_tty_port_activate(struct tty_port *port,
				       struct tty_struct *tty)
{
	struct flux_cons_tty_session *sess =
		flux_cons_tty_session_from_port(port);
	struct winsize ws;
	long ret;

	ret = host_syscall(__NR_ioctl, sess->stdin_fd, TIOCGWINSZ, &ws);
	if (ret >= 0)
		tty_do_resize(tty, &ws);

	sess->input_thread = kthread_run(flux_cons_tty_input_thread_fn, sess,
					 "flux_cons_tty/%u", sess->line);
	if (IS_ERR(sess->input_thread)) {
		ret = PTR_ERR(sess->input_thread);
		sess->input_thread = NULL;
		return (int)ret;
	}

	return 0;
}

static void flux_cons_tty_port_shutdown(struct tty_port *port)
{
	struct flux_cons_tty_session *sess =
		flux_cons_tty_session_from_port(port);

	flux_cons_tty_session_stop_input(sess);
}

static const struct tty_port_operations flux_cons_tty_port_ops = {
	.activate = flux_cons_tty_port_activate,
	.shutdown = flux_cons_tty_port_shutdown,
};

static struct flux_cons_tty_session *
flux_cons_tty_session_lookup_line(unsigned int line)
{
	return xa_load(&flux_cons_tty_by_line, line);
}

static bool flux_cons_tty_host_fd_is_tty(long host_fd)
{
	struct winsize ws;

	return host_syscall(__NR_ioctl, host_fd, TIOCGWINSZ, &ws) >= 0;
}

static void flux_cons_tty_session_init_port(struct flux_cons_tty_session *sess)
{
	tty_port_init(&sess->port);
	sess->port.ops = &flux_cons_tty_port_ops;
}

#ifdef CONFIG_FLUX_RUNC
static void flux_cons_tty_host_fds_close(long fds[3])
{
	int i;

	for (i = 0; i < 3; i++) {
		if (fds[i] >= 0)
			host_syscall(__NR_close, fds[i]);
	}
}

static int flux_cons_tty_host_fds_open(u32 session_id, long fds[3])
{
	static const char *const names[3] = { "stdin", "stdout", "stderr" };
	static const int flags[3] = {
		O_RDONLY | O_CLOEXEC,
		O_WRONLY | O_CLOEXEC,
		O_WRONLY | O_CLOEXEC,
	};
	int i;

	for (i = 0; i < 3; i++) {
		fds[i] = flux_cons_host_open_file(session_id, names[i], flags[i]);
		if (fds[i] >= 0)
			continue;

		flux_cons_tty_host_fds_close(fds);
		return (int)fds[i];
	}

	return 0;
}

#endif

static int flux_cons_tty_session_store_locked(struct flux_cons_tty_session *sess,
					      bool store_id)
{
	int ret;

	if (store_id) {
		ret = xa_err(xa_store(&flux_cons_tty_by_id, sess->session_id,
				      sess, GFP_KERNEL));
		if (ret < 0)
			return ret;
	}

	ret = xa_err(xa_store(&flux_cons_tty_by_line, sess->line, sess,
			      GFP_KERNEL));
	if (ret < 0 && store_id)
		xa_erase(&flux_cons_tty_by_id, sess->session_id);

	return ret;
}

#ifdef CONFIG_FLUX_RUNC
static int
flux_cons_tty_session_create_dynamic(const struct flux_cons *cons,
				     struct flux_cons_tty_session **out)
{
	struct flux_cons_tty_session *sess;
	long fds[3] = { -1, -1, -1 };
	int line;
	int ret;

	line = ida_alloc_range(&flux_cons_tty_lines, 1,
			       FLUX_CONS_TTY_MAX_SESSIONS - 1, GFP_KERNEL);
	if (line < 0)
		return line;

	ret = flux_cons_tty_host_fds_open(cons->id, fds);
	if (ret < 0)
		goto err_line;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess) {
		ret = -ENOMEM;
		goto err_fds;
	}

	sess->session_id = cons->id;
	sess->line = (unsigned int)line;
	sess->stdin_fd = fds[0];
	sess->stdout_fd = fds[1];
	sess->stderr_fd = fds[2];
	flux_cons_tty_session_init_port(sess);

	mutex_lock(&flux_cons_tty_lock);
	ret = flux_cons_tty_session_store_locked(sess, true);
	mutex_unlock(&flux_cons_tty_lock);
	if (ret < 0)
		goto err_sess;

	*out = sess;
	return 0;

err_sess:
	tty_port_destroy(&sess->port);
	kfree(sess);
err_fds:
	flux_cons_tty_host_fds_close(fds);
err_line:
	ida_free(&flux_cons_tty_lines, line);
	return ret;
}
#endif

static int flux_cons_tty_session_prepare_boot(void)
{
	int ret = 0;

	mutex_lock(&flux_cons_tty_lock);
	if (!xa_load(&flux_cons_tty_by_line, flux_cons_tty0.line)) {
		flux_cons_tty_session_init_port(&flux_cons_tty0);
		ret = flux_cons_tty_session_store_locked(&flux_cons_tty0, false);
	}
	mutex_unlock(&flux_cons_tty_lock);

	return ret;
}

static int flux_cons_tty_install(struct tty_driver *driver,
				 struct tty_struct *tty)
{
	struct flux_cons_tty_session *sess;

	sess = flux_cons_tty_session_lookup_line((unsigned int)tty->index);
	if (!sess)
		return -ENODEV;

	tty->driver_data = sess;
	return tty_port_install(&sess->port, driver, tty);
}

static void flux_cons_tty_cleanup_session(struct flux_cons_tty_session *sess)
{
	if (!sess)
		return;

	if (sess->persistent) {
		flux_cons_tty_session_stop_input(sess);
		return;
	}

	mutex_lock(&flux_cons_tty_lock);
	xa_erase(&flux_cons_tty_by_id, sess->session_id);
	xa_erase(&flux_cons_tty_by_line, sess->line);
	ida_free(&flux_cons_tty_lines, (int)sess->line);
	mutex_unlock(&flux_cons_tty_lock);

	flux_cons_tty_session_stop_input(sess);
	tty_port_destroy(&sess->port);
	if (sess->stdin_fd >= 0)
		host_syscall(__NR_close, sess->stdin_fd);
	if (sess->stdout_fd >= 0)
		host_syscall(__NR_close, sess->stdout_fd);
	if (sess->stderr_fd >= 0)
		host_syscall(__NR_close, sess->stderr_fd);
	kfree(sess);
}

static void flux_cons_tty_cleanup(struct tty_struct *tty)
{
	struct flux_cons_tty_session *sess = tty->driver_data;

	tty->driver_data = NULL;
	flux_cons_tty_cleanup_session(sess);
}

static int flux_cons_tty_open(struct tty_struct *tty, struct file *file)
{
	struct flux_cons_tty_session *sess = tty->driver_data;

	if (!sess)
		return -ENODEV;

	return tty_port_open(&sess->port, tty, file);
}

static void flux_cons_tty_close(struct tty_struct *tty, struct file *file)
{
	struct flux_cons_tty_session *sess = tty->driver_data;

	if (!sess)
		return;

	tty_port_close(&sess->port, tty, file);
}

static void flux_cons_tty_hangup(struct tty_struct *tty)
{
	struct flux_cons_tty_session *sess = tty->driver_data;

	if (!sess)
		return;

	tty_port_hangup(&sess->port);
}

static unsigned int flux_cons_tty_write_room(struct tty_struct *tty)
{
	return FLUX_CONSOLE_INPUT_BUF_SIZE;
}

static unsigned int flux_cons_tty_chars_in_buffer(struct tty_struct *tty)
{
	return 0;
}

static ssize_t flux_cons_tty_write(struct tty_struct *tty, const u8 *buf,
				   size_t count)
{
	struct flux_cons_tty_session *sess = tty->driver_data;
	size_t off = 0;

	if (!sess || sess->stdout_fd < 0)
		return -EIO;

	while (off < count) {
		long ret = host_syscall(__NR_write, sess->stdout_fd, buf + off,
					count - off);

		if (ret < 0)
			return off ? (ssize_t)off : (ssize_t)ret;
		if (ret == 0)
			break;
		off += (size_t)ret;
	}

	return (ssize_t)off;
}

static int flux_cons_tty_resize(struct tty_struct *tty, struct winsize *ws)
{
	struct flux_cons_tty_session *sess = tty->driver_data;
	long ret;
	int err;

	err = tty_do_resize(tty, ws);
	if (err < 0)
		return err;

	if (!sess || sess->stdin_fd < 0)
		return 0;

	ret = host_syscall(__NR_ioctl, sess->stdin_fd, TIOCSWINSZ, ws);
	if (ret < 0)
		return (int)ret;

	return 0;
}

static const struct tty_operations flux_cons_tty_ops = {
	.install = flux_cons_tty_install,
	.cleanup = flux_cons_tty_cleanup,
	.open = flux_cons_tty_open,
	.close = flux_cons_tty_close,
	.hangup = flux_cons_tty_hangup,
	.write_room = flux_cons_tty_write_room,
	.chars_in_buffer = flux_cons_tty_chars_in_buffer,
	.write = flux_cons_tty_write,
	.resize = flux_cons_tty_resize,
};

static struct file *flux_cons_tty_file_open(unsigned int line, int flags)
{
	struct path root;
	struct inode *inode;
	struct file *file;
	dev_t dev;
	int ret;

	if (!flux_cons_tty_driver)
		return ERR_PTR(-ENODEV);

	dev = MKDEV(flux_cons_tty_driver->major,
		    flux_cons_tty_driver->minor_start + line);
	get_fs_root(current->fs, &root);

	inode = new_inode_pseudo(root.dentry->d_sb);
	if (!inode) {
		path_put(&root);
		return ERR_PTR(-ENOMEM);
	}

	inode->i_ino = get_next_ino();
	init_special_inode(inode, S_IFCHR | 0600, dev);
	file = alloc_file_pseudo(inode, root.mnt, FLUX_CONS_TTY_NAME, flags,
				 &flux_cons_tty_file_fops);
	path_put(&root);
	if (IS_ERR(file))
		return file;

	ret = file->f_op->open(file_inode(file), file);
	if (ret < 0) {
		fput(file);
		return ERR_PTR(ret);
	}

	return file;
}

static int flux_cons_install_tty_files(struct flux_cons_tty_session *sess,
				       u32 session_id)
{
	static const int flags[3] = {
		O_RDONLY | O_CLOEXEC,
		O_WRONLY | O_CLOEXEC | O_NOCTTY,
		O_WRONLY | O_CLOEXEC | O_NOCTTY,
	};
	struct file *tty_files[3] = { NULL, NULL, NULL };
	bool opened = false;
	int ret;
	int fd;

	ret = ksys_setsid();
	if (ret < 0 && ret != -EPERM)
		return ret;

	for (fd = 0; fd < 3; fd++) {
		tty_files[fd] = flux_cons_tty_file_open(sess->line, flags[fd]);
		if (IS_ERR(tty_files[fd])) {
			ret = PTR_ERR(tty_files[fd]);
			tty_files[fd] = NULL;
			goto out;
		}
		opened = true;
	}

	return flux_cons_install_files(tty_files, session_id);

out:
	for (fd = 0; fd < 3; fd++) {
		if (tty_files[fd])
			fput(tty_files[fd]);
	}
	if (!opened)
		flux_cons_tty_cleanup_session(sess);
	return ret;
}

static long flux_cons_file_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct flux_cons_file_ctx *file_ctx = file->private_data;

	if (!file_ctx || file_ctx->host_fd < 0)
		return -EBADF;

	return host_syscall(__NR_ioctl, file_ctx->host_fd, cmd, arg);
}

static __poll_t flux_cons_file_poll(struct file *file,
				    struct poll_table_struct *wait)
{
	struct flux_cons_file_ctx *file_ctx = file->private_data;
	struct pollfd pfd = {};
	long ret;

	if (!file_ctx || file_ctx->host_fd < 0)
		return EPOLLERR;

	if (file_ctx->input) {
		struct flux_cons_input_buf *input = file_ctx->input;
		__poll_t mask = 0;

		poll_wait(file, &input->read_waitq, wait);

		if (input->count > 0)
			mask |= EPOLLIN;
		if (input->eof)
			mask |= EPOLLHUP;
		return mask;
	}

	pfd.fd = (int)file_ctx->host_fd;
	pfd.events = POLLIN | POLLOUT | POLLPRI | POLLERR | POLLHUP;
	ret = host_syscall(__NR_poll, &pfd, 1, 0);
	if (ret < 0)
		return EPOLLERR;

	return (__poll_t)pfd.revents;
}

static int flux_cons_file_release(struct inode *inode, struct file *file)
{
	struct flux_cons_file_ctx *file_ctx = file->private_data;

	if (file_ctx) {
		if (file_ctx->input) {
			kthread_stop(file_ctx->input->thread);
			kfree(file_ctx->input);
			file_ctx->input = NULL;
		}
		if (file_ctx->close_on_release && file_ctx->host_fd >= 0)
			host_syscall(__NR_close, file_ctx->host_fd);
		kfree(file_ctx);
		file->private_data = NULL;
	}

	return 0;
}

static const struct file_operations flux_cons_file_fops = {
	.read = flux_cons_file_read,
	.write = flux_cons_file_write,
	.unlocked_ioctl = flux_cons_file_ioctl,
	.poll = flux_cons_file_poll,
	.release = flux_cons_file_release,
	.llseek = no_llseek,
};

#ifdef CONFIG_FLUX_RUNC

static struct flux_cons flux_cons_dynamic;

static bool flux_cons_is_dynamic_stdio(u32 session_id)
{
	return session_id >= FLUX_EXEC_SESSION_DYNAMIC_STDIO_BASE &&
	       session_id < FLUX_EXEC_SESSION_DYNAMIC_TTY_BASE;
}

static bool flux_cons_is_dynamic_tty(u32 session_id)
{
	return session_id >= FLUX_EXEC_SESSION_DYNAMIC_TTY_BASE;
}

static int flux_cons_path(char *buf, size_t size, const char *root,
			  u32 session_id, const char *name)
{
	if (snprintf(buf, size, "%s/%s/%u/%s", root, FLUX_RUNC_SESSION_DIR,
		     session_id, name) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

static int flux_cons_host_getuid(uid_t *uid_out)
{
	long ret;

	if (!uid_out)
		return -EINVAL;

	ret = host_syscall(__NR_getuid);
	if (ret < 0)
		return (int)ret;

	*uid_out = (uid_t)ret;
	return 0;
}

static int flux_cons_host_try_open_file(const char *root, u32 session_id,
					const char *name, int flags)
{
	char *path;
	int ret;

	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	ret = flux_cons_path(path, PATH_MAX, root, session_id, name);
	if (ret < 0)
		goto out;

	ret = (int)host_syscall(__NR_openat, AT_FDCWD, path, flags, 0);
out:
	kfree(path);
	return ret;
}

static int flux_cons_host_open_file(u32 session_id, const char *name, int flags)
{
	char *user_root = NULL;
	uid_t uid;
	int ret;

	ret = flux_cons_host_try_open_file(FLUX_RUNC_STATE_ROOT, session_id,
					   name, flags);
	if (ret >= 0 || ret != -ENOENT)
		return ret;

	ret = flux_cons_host_getuid(&uid);
	if (ret < 0)
		return ret;

	user_root = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!user_root)
		return -ENOMEM;

	if (snprintf(user_root, PATH_MAX, FLUX_RUNC_USER_STATE_ROOT_FMT,
		     (unsigned long)uid) >= PATH_MAX) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	ret = flux_cons_host_try_open_file(user_root, session_id, name, flags);
out:
	kfree(user_root);
	return ret;
}

#endif /* CONFIG_FLUX_RUNC */

static struct file *flux_cons_file_wrap(long host_fd, const char *file_name,
					int flags, bool close_on_release,
					bool pump_input)
{
	struct flux_cons_file_ctx *file_ctx;
	struct file *file;

	if (host_fd < 0)
		return ERR_PTR(host_fd);

	file_ctx = kzalloc(sizeof(*file_ctx), GFP_KERNEL);
	if (!file_ctx)
		return ERR_PTR(-ENOMEM);

	file_ctx->host_fd = host_fd;
	file_ctx->close_on_release = close_on_release;
	if (pump_input) {
		file_ctx->input = flux_cons_input_buf_create(host_fd);
		if (IS_ERR(file_ctx->input)) {
			long ret = PTR_ERR(file_ctx->input);

			if (close_on_release)
				host_syscall(__NR_close, host_fd);
			kfree(file_ctx);
			return ERR_PTR(ret);
		}
	}
	file = anon_inode_getfile(file_name, &flux_cons_file_fops, file_ctx,
				  flags);
	if (IS_ERR(file)) {
		if (file_ctx->input) {
			kthread_stop(file_ctx->input->thread);
			kfree(file_ctx->input);
		}
		if (close_on_release)
			host_syscall(__NR_close, host_fd);
		kfree(file_ctx);
		return file;
	}

	return file;
}

#ifdef CONFIG_FLUX_RUNC
static struct file *flux_cons_file_create(u32 session_id, const char *path_name,
					  const char *file_name, int flags)
{
	long host_fd;

	host_fd = flux_cons_host_open_file(session_id, path_name, flags);
	return flux_cons_file_wrap(host_fd, file_name, flags, true,
				   !strcmp(path_name, "stdin"));
}

#endif /* CONFIG_FLUX_RUNC */

static int flux_cons_install_files(struct file *files[3], u32 session_id)
{
	int ret = 0;
	int fd;

	for (fd = 0; fd < 3; fd++) {
		ret = replace_fd((unsigned int)fd, files[fd], 0);
		if (ret < 0) {
			pr_err("failed to install session %u fd %d: %d\n",
			       session_id, fd, ret);
			break;
		}
	}

	for (fd = 0; fd < 3; fd++) {
		if (files[fd])
			fput(files[fd]);
	}

	return ret;
}

#ifdef CONFIG_FLUX_RUNC
static int flux_cons_install_host_stdio(const struct flux_cons *cons)
{
	struct file *stdio[3] = { NULL, NULL, NULL };
	int ret = 0;
	int fd;

	stdio[0] = flux_cons_file_create(cons->id, "stdin", "flux-stdin",
					 O_RDONLY | O_CLOEXEC);
	if (IS_ERR(stdio[0])) {
		ret = PTR_ERR(stdio[0]);
		stdio[0] = NULL;
		pr_err("failed to open stdio session %u stdin: %d\n", cons->id,
		       ret);
		goto out;
	}

	stdio[1] = flux_cons_file_create(cons->id, "stdout", "flux-stdout",
					 O_WRONLY | O_CLOEXEC);
	if (IS_ERR(stdio[1])) {
		ret = PTR_ERR(stdio[1]);
		stdio[1] = NULL;
		pr_err("failed to open stdio session %u stdout: %d\n", cons->id,
		       ret);
		goto out;
	}

	stdio[2] = flux_cons_file_create(cons->id, "stderr", "flux-stderr",
					 O_WRONLY | O_CLOEXEC);
	if (IS_ERR(stdio[2])) {
		ret = PTR_ERR(stdio[2]);
		stdio[2] = NULL;
		pr_err("failed to open stdio session %u stderr: %d\n", cons->id,
		       ret);
		goto out;
	}

	ret = flux_cons_install_files(stdio, cons->id);
	return ret;
out:
	for (fd = 0; fd < 3; fd++) {
		if (stdio[fd])
			fput(stdio[fd]);
	}
	return ret;
}

static int flux_cons_install_host_tty(const struct flux_cons *cons)
{
	struct flux_cons_tty_session *sess;
	int ret;

	if (!flux_cons_tty_driver)
		return -ENODEV;

	mutex_lock(&flux_cons_tty_lock);
	sess = xa_load(&flux_cons_tty_by_id, cons->id);
	mutex_unlock(&flux_cons_tty_lock);

	if (!sess) {
		ret = flux_cons_tty_session_create_dynamic(cons, &sess);
		if (ret < 0)
			return ret;
	}

	return flux_cons_install_tty_files(sess, cons->id);
}

#endif /* CONFIG_FLUX_RUNC */

int flux_cons_install_boot_stdio(void)
{
	static const char *const file_names[3] = {
		"flux-boot-stdin",
		"flux-boot-stdout",
		"flux-boot-stderr",
	};
	static int const oflags[3] = {
		O_RDONLY | O_CLOEXEC,
		O_WRONLY | O_CLOEXEC,
		O_WRONLY | O_CLOEXEC,
	};
	long host_fds[3] = {
		flux_cons_backend0.stdin_fd,
		flux_cons_backend0.stdout_fd,
		flux_cons_backend0.stderr_fd,
	};
	struct file *stdio[3] = { NULL, NULL, NULL };
	int ret;

	if (flux_cons_tty_driver && flux_cons_tty_host_fd_is_tty(0)) {
		ret = flux_cons_tty_session_prepare_boot();
		if (ret < 0)
			return ret;

		ret = flux_cons_install_tty_files(&flux_cons_tty0,
						  FLUX_EXEC_SESSION_NONE);
		if (ret >= 0)
			return 0;
	}

	stdio[0] = flux_cons_file_wrap(host_fds[0], file_names[0], oflags[0],
				       false, true);
	stdio[1] = flux_cons_file_wrap(host_fds[1], file_names[1], oflags[1],
				       false, false);
	stdio[2] = flux_cons_file_wrap(host_fds[2], file_names[2], oflags[2],
				       false, false);
	if (IS_ERR(stdio[0]) || IS_ERR(stdio[1]) || IS_ERR(stdio[2])) {
		ret = IS_ERR(stdio[0]) ? PTR_ERR(stdio[0]) :
		      IS_ERR(stdio[1]) ? PTR_ERR(stdio[1]) :
					 PTR_ERR(stdio[2]);
		if (!IS_ERR_OR_NULL(stdio[0]))
			fput(stdio[0]);
		if (!IS_ERR_OR_NULL(stdio[1]))
			fput(stdio[1]);
		if (!IS_ERR_OR_NULL(stdio[2]))
			fput(stdio[2]);
		return ret;
	}

	return flux_cons_install_files(stdio, FLUX_EXEC_SESSION_NONE);
}

const struct flux_cons *flux_cons_lookup(u32 session_id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(flux_cons_table); i++) {
		if (flux_cons_table[i].id == session_id)
			return &flux_cons_table[i];
	}

#ifdef CONFIG_FLUX_RUNC
	if (flux_cons_is_dynamic_stdio(session_id)) {
		flux_cons_dynamic.id = session_id;
		flux_cons_dynamic.kind = FLUX_CONS_KIND_STDIO;
		return &flux_cons_dynamic;
	}

	if (flux_cons_is_dynamic_tty(session_id)) {
		flux_cons_dynamic.id = session_id;
		flux_cons_dynamic.kind = FLUX_CONS_KIND_TTY;
		return &flux_cons_dynamic;
	}
#endif

	return NULL;
}

int flux_cons_install_stdio(const struct flux_cons *cons)
{
	if (!cons)
		return -EINVAL;

	switch (cons->id) {
	case FLUX_EXEC_SESSION_NONE:
		return 0;
#ifdef CONFIG_FLUX_RUNC
	default:
		if (cons->kind == FLUX_CONS_KIND_STDIO)
			return flux_cons_install_host_stdio(cons);
		return -EINVAL;
#endif
	}

	return -EINVAL;
}

int flux_cons_install_tty(const struct flux_cons *cons)
{
	if (!cons)
		return -EINVAL;

#ifdef CONFIG_FLUX_RUNC
	if (cons->kind == FLUX_CONS_KIND_TTY &&
	    flux_cons_is_dynamic_tty(cons->id))
		return flux_cons_install_host_tty(cons);
#endif

	return -EINVAL;
}

/*
 * Kernel console for printk output
 */
static void flux_cons0_write(struct console *con, const char *str, unsigned len)
{
	flux_cons_write_host(flux_cons_backend0.stdout_fd, str, len);
}

static struct console flux_cons0 = {
	.name = "flux_cons0",
	.write = flux_cons0_write,
	.flags = CON_PRINTBUFFER,
	.index = -1,
};

/*
 * Initialize kernel console for printk
 * This runs early to capture boot messages
 */
int __init flux_cons_init(void)
{
	int ret;

	flux_cons_tty_driver =
		tty_alloc_driver(FLUX_CONS_TTY_MAX_SESSIONS,
				 TTY_DRIVER_DYNAMIC_DEV | TTY_DRIVER_REAL_RAW);
	if (IS_ERR(flux_cons_tty_driver))
		return PTR_ERR(flux_cons_tty_driver);

	flux_cons_tty_driver->driver_name = FLUX_CONS_TTY_NAME;
	flux_cons_tty_driver->name = FLUX_CONS_TTY_NAME;
	flux_cons_tty_driver->type = TTY_DRIVER_TYPE_CONSOLE;
	flux_cons_tty_driver->subtype = SYSTEM_TYPE_CONSOLE;
	flux_cons_tty_driver->init_termios = tty_std_termios;
	flux_cons_tty_driver->init_termios.c_lflag &= ~ECHO;
	tty_set_operations(flux_cons_tty_driver, &flux_cons_tty_ops);
	tty_default_fops(&flux_cons_tty_file_fops);

	ret = tty_register_driver(flux_cons_tty_driver);
	if (ret < 0) {
		tty_driver_kref_put(flux_cons_tty_driver);
		flux_cons_tty_driver = NULL;
		return ret;
	}

	register_console(&flux_cons0);
	return 0;
}
core_initcall(flux_cons_init);
