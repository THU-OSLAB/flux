// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/statfs.h>
#include <linux/dirent.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/magic.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/highmem.h>
#include <linux/overflow.h>
#include <asm/host_fs.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>

#define FLUX_HOSTFS_NAME "flux_hostfs"
#define FLUX_HOSTFS_DIRBUF_SIZE 4096
#define FLUX_HOSTFS_DIRECT_IO_ALIGN 512

struct flux_hostfs_super {
	char *root;
	bool sysfs;
};

static const char *flux_hostfs_root(struct super_block *sb)
{
	return ((struct flux_hostfs_super *)sb->s_fs_info)->root;
}

/*
 * The inode owns backing descriptors for page-cache I/O, including writeback
 * after the last application descriptor has closed. Per-open descriptors keep
 * host O_DIRECT state separate from ordinary page-cache reads and writes.
 */
struct flux_hostfs_inode {
	struct inode inode;
	struct mutex open_lock;
	int read_fd;
	int write_fd;
};

static struct flux_hostfs_inode *flux_hostfs_i(struct inode *inode)
{
	return container_of(inode, struct flux_hostfs_inode, inode);
}

static struct inode *flux_hostfs_alloc_inode(struct super_block *sb)
{
	struct flux_hostfs_inode *hi = kzalloc(sizeof(*hi), GFP_KERNEL);

	if (!hi)
		return NULL;
	inode_init_once(&hi->inode);
	mutex_init(&hi->open_lock);
	hi->read_fd = hi->write_fd = -1;
	return &hi->inode;
}

static void flux_hostfs_free_inode(struct inode *inode)
{
	kfree(flux_hostfs_i(inode));
}

static const struct address_space_operations flux_hostfs_aops;

static const struct inode_operations flux_hostfs_file_iops;
static const struct inode_operations flux_hostfs_dir_iops;
static const struct inode_operations flux_hostfs_symlink_iops;
static const struct file_operations flux_hostfs_file_fops;
static const struct file_operations flux_hostfs_sysfs_fops;
static const struct file_operations flux_hostfs_dir_fops;

static char *flux_hostfs_sanitize_root(const char *root)
{
	char *out;
	size_t len;

	if (!root || !*root)
		return kstrdup("/", GFP_KERNEL);

	len = strlen(root);
	while (len > 1 && root[len - 1] == '/')
		len--;

	out = kmalloc(len + 1, GFP_KERNEL);
	if (!out)
		return NULL;

	memcpy(out, root, len);
	out[len] = '\0';
	return out;
}

static char *flux_hostfs_dentry_path(struct dentry *dentry)
{
	struct super_block *sb = dentry->d_sb;
	const char *root = flux_hostfs_root(sb);
	char *buf = __getname();
	char *p;
	size_t root_len;

	if (!buf)
		return NULL;

	p = dentry_path_raw(dentry, buf, PATH_MAX);
	if (IS_ERR(p)) {
		__putname(buf);
		return NULL;
	}

	root_len = strlen(root);
	if (root_len == 0 || (root_len == 1 && root[0] == '/')) {
		memmove(buf, p, strlen(p) + 1);
		return buf;
	}

	if (root_len + strlen(p) + 1 >= PATH_MAX) {
		__putname(buf);
		return NULL;
	}

	memmove(buf + root_len, p, strlen(p) + 1);
	memcpy(buf, root, root_len);
	return buf;
}

static int flux_hostfs_statx(const char *path, struct statx *stx, int flags)
{
	return host_syscall(__NR_statx, AT_FDCWD, path,
			    AT_STATX_SYNC_AS_STAT | flags, STATX_BASIC_STATS,
			    stx);
}

static void flux_hostfs_statx_to_kstat(const struct statx *stx,
				       struct kstat *stat)
{
	stat->result_mask = STATX_BASIC_STATS;
	stat->mode = stx->stx_mode;
	stat->uid = make_kuid(&init_user_ns, stx->stx_uid);
	stat->gid = make_kgid(&init_user_ns, stx->stx_gid);
	stat->nlink = stx->stx_nlink;
	stat->size = stx->stx_size;
	stat->blocks = stx->stx_blocks;
	stat->blksize = stx->stx_blksize;
	stat->ino = stx->stx_ino;
	stat->rdev = MKDEV(stx->stx_rdev_major, stx->stx_rdev_minor);
	stat->dev = MKDEV(stx->stx_dev_major, stx->stx_dev_minor);
	stat->atime = (struct timespec64){ stx->stx_atime.tv_sec,
					   stx->stx_atime.tv_nsec };
	stat->mtime = (struct timespec64){ stx->stx_mtime.tv_sec,
					   stx->stx_mtime.tv_nsec };
	stat->ctime = (struct timespec64){ stx->stx_ctime.tv_sec,
					   stx->stx_ctime.tv_nsec };
}

static void flux_hostfs_inode_update(struct inode *inode,
				     const struct statx *stx)
{
	inode->i_mode = stx->stx_mode;
	inode->i_uid = make_kuid(&init_user_ns, stx->stx_uid);
	inode->i_gid = make_kgid(&init_user_ns, stx->stx_gid);
	set_nlink(inode, stx->stx_nlink);
	i_size_write(inode, stx->stx_size);
	inode->i_blocks = stx->stx_blocks;
	inode->i_atime = (struct timespec64){ stx->stx_atime.tv_sec,
					      stx->stx_atime.tv_nsec };
	inode->i_mtime = (struct timespec64){ stx->stx_mtime.tv_sec,
					      stx->stx_mtime.tv_nsec };
	inode_set_ctime(inode, stx->stx_ctime.tv_sec, stx->stx_ctime.tv_nsec);
}

static struct inode *flux_hostfs_inode_from_statx(struct super_block *sb,
						  const struct statx *stx)
{
	unsigned long ino = stx->stx_ino ? stx->stx_ino : get_next_ino();
	struct inode *inode;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW)) {
		spin_lock(&inode->i_lock);
		flux_hostfs_inode_update(inode, stx);
		spin_unlock(&inode->i_lock);
		return inode;
	}

	flux_hostfs_inode_update(inode, stx);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &flux_hostfs_dir_iops;
		inode->i_fop = &flux_hostfs_dir_fops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &flux_hostfs_symlink_iops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &flux_hostfs_file_iops;
		if (((struct flux_hostfs_super *)sb->s_fs_info)->sysfs) {
			inode->i_fop = &flux_hostfs_sysfs_fops;
		} else {
			inode->i_fop = &flux_hostfs_file_fops;
			inode->i_mapping->a_ops = &flux_hostfs_aops;
		}
	} else {
		init_special_inode(inode, inode->i_mode,
				   MKDEV(stx->stx_rdev_major,
					 stx->stx_rdev_minor));
		inode->i_op = &flux_hostfs_file_iops;
	}

	unlock_new_inode(inode);
	return inode;
}

static int flux_hostfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			       struct kstat *stat, u32 request_mask,
			       unsigned int flags)
{
	struct statx stx;
	char *host_path;
	int statx_flags = 0;
	int err;

	if (flags & AT_SYMLINK_NOFOLLOW)
		statx_flags |= AT_SYMLINK_NOFOLLOW;

	host_path = flux_hostfs_dentry_path(path->dentry);
	if (!host_path)
		return -ENOMEM;

	err = flux_hostfs_statx(host_path, &stx, statx_flags);
	__putname(host_path);
	if (err)
		return err;

	flux_hostfs_statx_to_kstat(&stx, stat);
	return 0;
}

static int flux_hostfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			       struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	long host_fd = -1;
	char *path;
	int err;

	err = setattr_prepare(&nop_mnt_idmap, dentry, attr);
	if (err)
		return err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	if (attr->ia_valid & ATTR_MODE) {
		err = host_syscall(__NR_chmod, path, attr->ia_mode);
		if (err)
			goto out;
	}

	if (attr->ia_valid & (ATTR_UID | ATTR_GID)) {
		uid_t uid = (attr->ia_valid & ATTR_UID) ?
				    from_kuid(&init_user_ns, attr->ia_uid) :
				    (uid_t)-1;
		gid_t gid = (attr->ia_valid & ATTR_GID) ?
				    from_kgid(&init_user_ns, attr->ia_gid) :
				    (gid_t)-1;
		err = host_syscall(__NR_chown, path, uid, gid);
		if (err)
			goto out;
	}

	if (attr->ia_valid & ATTR_SIZE) {
		err = filemap_write_and_wait(inode->i_mapping);
		if (err)
			goto out;
		if ((attr->ia_valid & ATTR_FILE) && attr->ia_file) {
			host_fd = flux_hostfs_get_host_fd(attr->ia_file);
			if (host_fd < 0) {
				err = host_fd;
				goto out;
			}
			err = host_syscall(__NR_ftruncate, host_fd, attr->ia_size);
		} else {
			err = host_syscall(__NR_truncate, path, attr->ia_size);
		}
		if (err)
			goto out;
		if (attr->ia_size != i_size_read(inode))
			truncate_setsize(inode, attr->ia_size);
	}

	if (attr->ia_valid &
	    (ATTR_ATIME | ATTR_MTIME | ATTR_ATIME_SET | ATTR_MTIME_SET)) {
		struct timespec64 ts[2] = {
			{ .tv_sec = 0, .tv_nsec = UTIME_OMIT },
			{ .tv_sec = 0, .tv_nsec = UTIME_OMIT },
		};

		if (attr->ia_valid & (ATTR_ATIME | ATTR_ATIME_SET))
			ts[0] = attr->ia_atime;
		if (attr->ia_valid & (ATTR_MTIME | ATTR_MTIME_SET))
			ts[1] = attr->ia_mtime;

		if (host_fd >= 0)
			err = host_syscall(__NR_utimensat, host_fd, "", ts,
					   AT_EMPTY_PATH);
		else
			err = host_syscall(__NR_utimensat, AT_FDCWD, path, ts, 0);
		if (err)
			goto out;
	}

	setattr_copy(&nop_mnt_idmap, inode, attr);
	mark_inode_dirty(inode);
	err = 0;

out:
	__putname(path);
	return err;
}

static int flux_hostfs_cache_fd(struct inode *inode, int fd, fmode_t mode)
{
	struct flux_hostfs_inode *hi = flux_hostfs_i(inode);
	char path[48];
	int ret = 0;

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	mutex_lock(&hi->open_lock);
	if ((mode & FMODE_READ) && hi->read_fd < 0) {
		ret = host_syscall(__NR_openat, AT_FDCWD, path,
				   O_RDONLY | O_CLOEXEC, 0);
		if (ret < 0)
			goto out;
		hi->read_fd = ret;
	}
	if ((mode & FMODE_WRITE) && hi->write_fd < 0) {
		ret = host_syscall(__NR_openat, AT_FDCWD, path,
				   O_WRONLY | O_CLOEXEC, 0);
		if (ret < 0)
			goto out;
		hi->write_fd = ret;
	}
	ret = 0;
out:
	mutex_unlock(&hi->open_lock);
	return ret;
}

static int flux_hostfs_open(struct inode *inode, struct file *file)
{
	char *path;
	int flags = file->f_flags;
	long fd;

	path = flux_hostfs_dentry_path(file->f_path.dentry);
	if (!path)
		return -ENOMEM;

	flags &= ~(O_CREAT | O_EXCL);
	if (S_ISDIR(inode->i_mode))
		flags |= O_DIRECTORY;

	fd = host_syscall(__NR_openat, AT_FDCWD, path, flags, 0);
	__putname(path);
	if (fd < 0)
		return fd;

	if (S_ISREG(inode->i_mode) && file->f_op != &flux_hostfs_sysfs_fops) {
		int ret = flux_hostfs_cache_fd(inode, fd, file->f_mode);

		if (ret) {
			host_syscall(__NR_close, fd);
			return ret;
		}
	}
	file->private_data = (void *)(long)fd;
	if (file->f_mode & FMODE_READ)
		file->f_mode |= FMODE_CAN_READ;
	if (file->f_mode & FMODE_WRITE)
		file->f_mode |= FMODE_CAN_WRITE;
	if (file->f_flags & O_DIRECT)
		file->f_mode |= FMODE_CAN_ODIRECT;
	return 0;
}

long flux_hostfs_get_host_fd(struct file *filp)
{
	struct super_block *sb;
	long ret;

	sb = filp->f_path.dentry->d_sb;
	if (!sb || !sb->s_type || !sb->s_type->name) {
		ret = -EINVAL;
		goto out;
	}

	if (strcmp(sb->s_type->name, FLUX_HOSTFS_NAME) != 0) {
		ret = -EINVAL;
		goto out;
	}

	ret = (long)filp->private_data;
	if (ret < 0)
		ret = -EBADF;

out:
	return ret;
}

long flux_hostfs_get_host_fd_from_fd(int fd)
{
	struct file *filp;
	long ret;

	filp = fget(fd);
	if (!filp)
		return -EBADF;

	ret = flux_hostfs_get_host_fd(filp);
	fput(filp);
	return ret;
}

static int flux_hostfs_release(struct inode *inode, struct file *file)
{
	long fd = (long)file->private_data;

	if (fd >= 0)
		host_syscall(__NR_close, fd);
	file->private_data = (void *)-1;
	return 0;
}

static bool flux_hostfs_iter_direct_io_aligned(const struct iov_iter *iter,
					       size_t len, loff_t pos)
{
	return !(iov_iter_alignment(iter) & (FLUX_HOSTFS_DIRECT_IO_ALIGN - 1)) &&
	       !(len & (FLUX_HOSTFS_DIRECT_IO_ALIGN - 1)) &&
	       !(pos & (FLUX_HOSTFS_DIRECT_IO_ALIGN - 1));
}

static int flux_hostfs_read_folio(struct file *file, struct folio *folio)
{
	struct flux_hostfs_inode *hi = flux_hostfs_i(folio->mapping->host);
	void *buf = kmap_local_folio(folio, 0);
	size_t size = folio_size(folio), done = 0;
	loff_t pos = folio_pos(folio);
	long ret = 0;

	while (done < size) {
		ret = host_syscall(__NR_pread64, hi->read_fd,
				   buf + done, size - done, pos + done);
		if (ret <= 0)
			break;
		done += ret;
	}
	if (ret < 0) {
		folio_set_error(folio);
	} else {
		memset(buf + done, 0, size - done);
		folio_mark_uptodate(folio);
		ret = 0;
	}
	flush_dcache_folio(folio);
	kunmap_local(buf);
	folio_unlock(folio);
	return ret;
}

static int flux_hostfs_writepage(struct page *page,
				 struct writeback_control *wbc)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	loff_t pos = page_offset(page), size = i_size_read(inode);
	size_t len = size > pos ? min_t(loff_t, PAGE_SIZE, size - pos) : 0;
	size_t done = 0;
	void *buf = kmap_local_page(page);
	long ret = 0;

	set_page_writeback(page);
	while (done < len) {
		ret = host_syscall(__NR_pwrite64, flux_hostfs_i(inode)->write_fd,
				   buf + done, len - done, pos + done);
		if (ret <= 0) {
			ret = ret ?: -EIO;
			mapping_set_error(mapping, ret);
			redirty_page_for_writepage(wbc, page);
			break;
		}
		done += ret;
	}
	kunmap_local(buf);
	end_page_writeback(page);
	unlock_page(page);
	return ret < 0 ? ret : 0;
}

/* As in Linux hostfs: partial writes reach the backing file before unlock.
 * An incomplete, previously uncached page stays !uptodate and is read back
 * through read_folio; an existing mapped page is updated under its page lock.
 */
static int flux_hostfs_write_begin(struct file *file,
				  struct address_space *mapping, loff_t pos,
				  unsigned len, struct page **pagep,
				  void **fsdata)
{
	*pagep = grab_cache_page_write_begin(mapping, pos >> PAGE_SHIFT);
	return *pagep ? 0 : -ENOMEM;
}

static int flux_hostfs_write_end(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned len, unsigned copied,
				struct page *page, void *fsdata)
{
	void *buf = kmap_local_page(page);
	long ret = host_syscall(__NR_pwrite64,
			       flux_hostfs_i(mapping->host)->write_fd,
			       buf + offset_in_page(pos), copied, pos);

	kunmap_local(buf);
	if (ret == PAGE_SIZE)
		SetPageUptodate(page);
	if (ret > 0 && pos + ret > i_size_read(mapping->host))
		i_size_write(mapping->host, pos + ret);
	unlock_page(page);
	put_page(page);
	return ret;
}

/* Generic file I/O owns cache flush/invalidation and offset accounting. */
static ssize_t flux_hostfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	bool write = iov_iter_rw(iter) == WRITE;
	size_t len = iov_iter_count(iter), done = 0;
	loff_t pos = iocb->ki_pos;
	long fd = (long)iocb->ki_filp->private_data, err = 0;
	char *buf;

	if (!flux_hostfs_iter_direct_io_aligned(iter, len, pos))
		return -EINVAL;
	buf = (char *)__get_free_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	while (done < len) {
		size_t chunk = min_t(size_t, len - done, PAGE_SIZE);
		size_t copied;
		long ret;

		if (write) {
			copied = copy_from_iter(buf, chunk, iter);
			if (copied != chunk) {
				iov_iter_revert(iter, copied);
				err = -EFAULT;
				break;
			}
		}
		ret = host_syscall(write ? __NR_pwrite64 : __NR_pread64,
				   fd, buf, chunk, pos + done);
		if (write && ret < (long)chunk)
			iov_iter_revert(iter, chunk - max(ret, 0L));
		if (ret <= 0) {
			err = ret;
			break;
		}
		if (!write) {
			copied = copy_to_iter(buf, ret, iter);
			if (copied != ret) {
				done += copied;
				err = -EFAULT;
				break;
			}
		}
		done += ret;
		if (ret < chunk)
			break;
	}
	free_page((unsigned long)buf);
	return done ? done : err;
}

static const struct address_space_operations flux_hostfs_aops = {
	.read_folio = flux_hostfs_read_folio,
	.writepage = flux_hostfs_writepage,
	.dirty_folio = filemap_dirty_folio,
	.write_begin = flux_hostfs_write_begin,
	.write_end = flux_hostfs_write_end,
	.direct_IO = flux_hostfs_direct_IO,
};

static int flux_hostfs_fsync(struct file *file, loff_t start, loff_t end,
			     int datasync)
{
	long fd = (long)file->private_data;

	if (fd < 0)
		return -EBADF;

	if (S_ISREG(file_inode(file)->i_mode)) {
		int ret = file_write_and_wait_range(file, start, end);

		if (ret)
			return ret;
	}
	return host_syscall(datasync ? __NR_fdatasync : __NR_fsync, fd);
}

static long flux_hostfs_fallocate(struct file *file, int mode, loff_t offset,
				  loff_t len)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	long fd = (long)file->private_data;
	struct statx stx;
	long ret;

	if (fd < 0)
		return -EBADF;
	inode_lock(inode);
	filemap_invalidate_lock(mapping);
	ret = filemap_write_and_wait(mapping);
	if (ret)
		goto out;
	ret = host_syscall(__NR_fallocate, fd, mode, offset, len, 0, 0);
	if (ret)
		goto out;
	/* Keep private COW pages; retire file-backed PTEs before cache removal. */
	unmap_mapping_range(mapping, 0, 0, 0);
	ret = invalidate_inode_pages2(mapping);
	if (ret)
		goto out;
	ret = host_syscall(__NR_statx, fd, "", AT_EMPTY_PATH,
			   STATX_SIZE, &stx);
	if (!ret)
		i_size_write(inode, stx.stx_size);
out:
	filemap_invalidate_unlock(mapping);
	inode_unlock(inode);
	return ret;
}

static int flux_hostfs_readdir(struct file *file, struct dir_context *ctx)
{
	long fd = (long)file->private_data;
	char *buf;
	int ret = 0;

	if (fd < 0)
		return -EBADF;

	buf = kmalloc(FLUX_HOSTFS_DIRBUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (;;) {
		long nread = host_syscall(__NR_getdents64, fd, buf,
					  FLUX_HOSTFS_DIRBUF_SIZE);
		unsigned long bpos = 0;

		if (nread < 0) {
			ret = nread;
			break;
		}
		if (nread == 0) {
			ret = 0;
			break;
		}

		while (bpos < nread) {
			struct linux_dirent64 *d =
				(struct linux_dirent64 *)(buf + bpos);

			if (!d->d_reclen) {
				ret = -EIO;
				goto out;
			}

			if (!dir_emit(ctx, d->d_name, strlen(d->d_name),
				      d->d_ino, d->d_type)) {
				ret = 0;
				goto out;
			}
			ctx->pos = d->d_off;
			bpos += d->d_reclen;
		}
	}

out:
	kfree(buf);
	return ret;
}

static const char *flux_hostfs_get_link(struct dentry *dentry,
					struct inode *inode,
					struct delayed_call *done)
{
	char *path;
	char *link;
	long ret;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	link = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!link)
		return ERR_PTR(-ENOMEM);

	path = flux_hostfs_dentry_path(dentry);
	if (!path) {
		kfree(link);
		return ERR_PTR(-ENOMEM);
	}

	ret = host_syscall(__NR_readlinkat, AT_FDCWD, path, link, PATH_MAX);
	__putname(path);
	if (ret < 0) {
		kfree(link);
		return ERR_PTR(ret);
	}
	if (ret >= PATH_MAX) {
		kfree(link);
		return ERR_PTR(-E2BIG);
	}
	link[ret] = '\0';
	set_delayed_call(done, kfree_link, link);
	return link;
}

static struct dentry *
flux_hostfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct statx stx;
	struct inode *inode;
	char *path;
	int err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return ERR_PTR(-ENOMEM);

	err = flux_hostfs_statx(path, &stx, AT_SYMLINK_NOFOLLOW);
	__putname(path);
	if (err) {
		if (err == -ENOENT) {
			d_set_d_op(dentry, &simple_dentry_operations);
			d_add(dentry, NULL);
			return NULL;
		}
		return ERR_PTR(err);
	}

	inode = flux_hostfs_inode_from_statx(dir->i_sb, &stx);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	return d_splice_alias(inode, dentry);
}

static int flux_hostfs_create(struct mnt_idmap *idmap, struct inode *dir,
			      struct dentry *dentry, umode_t mode, bool excl)
{
	char *path;
	struct statx stx;
	struct inode *inode;
	int flags = O_CREAT | O_RDWR;
	long fd;
	int err;

	if (excl)
		flags |= O_EXCL;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	fd = host_syscall(__NR_openat, AT_FDCWD, path, flags, mode);
	if (fd >= 0)
		host_syscall(__NR_close, fd);
	if (fd < 0) {
		__putname(path);
		return fd;
	}

	err = flux_hostfs_statx(path, &stx, AT_SYMLINK_NOFOLLOW);
	__putname(path);
	if (err)
		return err;

	inode = flux_hostfs_inode_from_statx(dir->i_sb, &stx);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int flux_hostfs_unlink(struct inode *dir, struct dentry *dentry)
{
	char *path;
	long err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	err = host_syscall(__NR_unlinkat, AT_FDCWD, path, 0);
	__putname(path);
	return err;
}

static int flux_hostfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			     struct dentry *dentry, umode_t mode)
{
	char *path;
	struct statx stx;
	struct inode *inode;
	long err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	err = host_syscall(__NR_mkdirat, AT_FDCWD, path, mode);
	if (err) {
		__putname(path);
		return err;
	}

	err = flux_hostfs_statx(path, &stx, AT_SYMLINK_NOFOLLOW);
	__putname(path);
	if (err)
		return err;

	inode = flux_hostfs_inode_from_statx(dir->i_sb, &stx);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int flux_hostfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	char *path;
	long err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	err = host_syscall(__NR_unlinkat, AT_FDCWD, path, AT_REMOVEDIR);
	__putname(path);
	return err;
}

static int flux_hostfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			       struct dentry *dentry, const char *target)
{
	char *path;
	struct statx stx;
	struct inode *inode;
	long err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	err = host_syscall(__NR_symlinkat, target, AT_FDCWD, path);
	if (err) {
		__putname(path);
		return err;
	}

	err = flux_hostfs_statx(path, &stx, AT_SYMLINK_NOFOLLOW);
	__putname(path);
	if (err)
		return err;

	inode = flux_hostfs_inode_from_statx(dir->i_sb, &stx);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int flux_hostfs_link(struct dentry *old_dentry, struct inode *dir,
			    struct dentry *dentry)
{
	char *old_path;
	char *new_path;
	struct statx stx;
	struct inode *inode;
	long err;

	old_path = flux_hostfs_dentry_path(old_dentry);
	if (!old_path)
		return -ENOMEM;

	new_path = flux_hostfs_dentry_path(dentry);
	if (!new_path) {
		__putname(old_path);
		return -ENOMEM;
	}

	err = host_syscall(__NR_linkat, AT_FDCWD, old_path, AT_FDCWD, new_path,
			   0);
	__putname(old_path);
	if (err) {
		__putname(new_path);
		return err;
	}

	err = flux_hostfs_statx(new_path, &stx, AT_SYMLINK_NOFOLLOW);
	__putname(new_path);
	if (err)
		return err;

	inode = flux_hostfs_inode_from_statx(dir->i_sb, &stx);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int flux_hostfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
			     struct dentry *dentry, umode_t mode, dev_t dev)
{
	char *path;
	struct statx stx;
	struct inode *inode;
	long err;

	path = flux_hostfs_dentry_path(dentry);
	if (!path)
		return -ENOMEM;

	err = host_syscall(__NR_mknodat, AT_FDCWD, path, mode, dev);
	if (err) {
		__putname(path);
		return err;
	}

	err = flux_hostfs_statx(path, &stx, AT_SYMLINK_NOFOLLOW);
	__putname(path);
	if (err)
		return err;

	inode = flux_hostfs_inode_from_statx(dir->i_sb, &stx);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static int flux_hostfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			      struct dentry *old_dentry, struct inode *new_dir,
			      struct dentry *new_dentry, unsigned int flags)
{
	char *old_path;
	char *new_path;
	long err;

	old_path = flux_hostfs_dentry_path(old_dentry);
	if (!old_path)
		return -ENOMEM;

	new_path = flux_hostfs_dentry_path(new_dentry);
	if (!new_path) {
		__putname(old_path);
		return -ENOMEM;
	}

	err = host_syscall(__NR_renameat2, AT_FDCWD, old_path, AT_FDCWD,
			   new_path, flags);
	if (err == -ENOSYS && flags == 0)
		err = host_syscall(__NR_renameat, AT_FDCWD, old_path, AT_FDCWD,
				   new_path);

	__putname(old_path);
	__putname(new_path);
	return err;
}

static const struct inode_operations flux_hostfs_file_iops = {
	.getattr = flux_hostfs_getattr,
	.setattr = flux_hostfs_setattr,
};

static const struct inode_operations flux_hostfs_dir_iops = {
	.lookup = flux_hostfs_lookup,
	.create = flux_hostfs_create,
	.unlink = flux_hostfs_unlink,
	.mkdir = flux_hostfs_mkdir,
	.rmdir = flux_hostfs_rmdir,
	.symlink = flux_hostfs_symlink,
	.link = flux_hostfs_link,
	.mknod = flux_hostfs_mknod,
	.rename = flux_hostfs_rename,
	.getattr = flux_hostfs_getattr,
	.setattr = flux_hostfs_setattr,
};

static const struct inode_operations flux_hostfs_symlink_iops = {
	.get_link = flux_hostfs_get_link,
	.getattr = flux_hostfs_getattr,
};

/* Sysfs attributes are generated by the backing kernel. Their nominal
 * i_size is not an EOF, and page caching would retain obsolete device state.
 * Keep each open's backing descriptor and forward one attribute operation;
 * in particular, do not pad short reads or split a write into callbacks.
 */
static ssize_t flux_hostfs_sysfs_read_iter(struct kiocb *iocb,
					struct iov_iter *iter)
{
	size_t len = min_t(size_t, iov_iter_count(iter), PAGE_SIZE);
	void *buf;
	long ret;

	if (!len)
		return 0;
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = host_syscall(__NR_pread64, (long)iocb->ki_filp->private_data,
			   buf, len, iocb->ki_pos);
	if (ret > 0) {
		size_t copied = copy_to_iter(buf, ret, iter);

		iocb->ki_pos += copied;
		ret = copied ?: -EFAULT;
	}
	kfree(buf);
	return ret;
}

static ssize_t flux_hostfs_sysfs_write_iter(struct kiocb *iocb,
					 struct iov_iter *iter)
{
	size_t len = min_t(size_t, iov_iter_count(iter), PAGE_SIZE);
	void *buf;
	long ret;

	if (!len)
		return 0;
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (!copy_from_iter_full(buf, len, iter)) {
		ret = -EFAULT;
		goto out;
	}
	ret = host_syscall(__NR_pwrite64, (long)iocb->ki_filp->private_data,
			   buf, len, iocb->ki_pos);
	if (ret > 0)
		iocb->ki_pos += ret;
	iov_iter_revert(iter, len - (ret > 0 ? ret : 0));
out:
	kfree(buf);
	return ret;
}

static loff_t flux_hostfs_sysfs_llseek(struct file *file, loff_t offset,
				     int whence)
{
	long ret;

	/* pread/pwrite track the Flux offset, not the backing descriptor offset. */
	if (whence == SEEK_CUR) {
		if (check_add_overflow(file->f_pos, offset, &offset))
			return -EINVAL;
		whence = SEEK_SET;
	}
	ret = host_syscall(__NR_lseek, (long)file->private_data, offset, whence);
	return ret < 0 ? ret : vfs_setpos(file, ret, MAX_LFS_FILESIZE);
}

static const struct file_operations flux_hostfs_sysfs_fops = {
	.llseek = flux_hostfs_sysfs_llseek,
	.read_iter = flux_hostfs_sysfs_read_iter,
	.write_iter = flux_hostfs_sysfs_write_iter,
	.open = flux_hostfs_open,
	.release = flux_hostfs_release,
};

static const struct file_operations flux_hostfs_file_fops = {
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.mmap = generic_file_mmap,
	.splice_read = filemap_splice_read,
	.splice_write = iter_file_splice_write,
	.open = flux_hostfs_open,
	.release = flux_hostfs_release,
	.fsync = flux_hostfs_fsync,
	.fallocate = flux_hostfs_fallocate,
};

static const struct file_operations flux_hostfs_dir_fops = {
	.llseek = generic_file_llseek,
	.iterate_shared = flux_hostfs_readdir,
	.read = generic_read_dir,
	.fsync = flux_hostfs_fsync,
	.open = flux_hostfs_open,
	.release = flux_hostfs_release,
};

static int flux_hostfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct statfs st;
	const char *root = flux_hostfs_root(dentry->d_sb);
	long err;

	err = host_syscall(__NR_statfs, root, &st);
	if (err)
		return err;

	buf->f_type = st.f_type;
	buf->f_bsize = st.f_bsize;
	buf->f_blocks = st.f_blocks;
	buf->f_bfree = st.f_bfree;
	buf->f_bavail = st.f_bavail;
	buf->f_files = st.f_files;
	buf->f_ffree = st.f_ffree;
	buf->f_fsid = st.f_fsid;
	buf->f_namelen = st.f_namelen;
	return 0;
}

static void flux_hostfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (flux_hostfs_i(inode)->read_fd >= 0)
		host_syscall(__NR_close, flux_hostfs_i(inode)->read_fd);
	if (flux_hostfs_i(inode)->write_fd >= 0)
		host_syscall(__NR_close, flux_hostfs_i(inode)->write_fd);
}

static const struct super_operations flux_hostfs_sb_ops = {
	.alloc_inode = flux_hostfs_alloc_inode,
	.free_inode = flux_hostfs_free_inode,
	.statfs = flux_hostfs_statfs,
	.drop_inode = generic_delete_inode,
	.evict_inode = flux_hostfs_evict_inode,
};

static int flux_hostfs_fill_super(struct super_block *sb, void *data,
				  int silent)
{
	struct flux_hostfs_super *info;
	struct inode *inode;
	struct statx stx;
	struct statfs st;
	int err;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->root = flux_hostfs_sanitize_root(data);
	if (!info->root) {
		kfree(info);
		return -ENOMEM;
	}

	sb->s_magic = HOSTFS_SUPER_MAGIC;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &flux_hostfs_sb_ops;
	sb->s_fs_info = info;

	/* Inspect the backing filesystem once at mount, outside regular I/O. */
	err = host_syscall(__NR_statfs, info->root, &st);
	if (err)
		goto fail;
	info->sysfs = st.f_type == SYSFS_MAGIC;
	err = flux_hostfs_statx(info->root, &stx, 0);
	if (err)
		goto fail;

	inode = flux_hostfs_inode_from_statx(sb, &stx);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto fail;
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto fail;
	}
	return 0;
fail:
	sb->s_fs_info = NULL;
	kfree(info->root);
	kfree(info);
	return err;
}

static struct dentry *flux_hostfs_mount(struct file_system_type *type,
					int flags, const char *dev_name,
					void *data)
{
	pr_info("flux_hostfs: mount host root '%s' (%s)\n",
		dev_name ? dev_name : "(null)",
		(flags & SB_RDONLY) ? "ro" : "rw");
	return mount_nodev(type, flags, (void *)dev_name,
			   flux_hostfs_fill_super);
}

static void flux_hostfs_kill_sb(struct super_block *sb)
{
	struct flux_hostfs_super *info = sb->s_fs_info;

	kill_anon_super(sb);
	if (info) {
		kfree(info->root);
		kfree(info);
	}
}

static struct file_system_type flux_hostfs_type = {
	.owner = THIS_MODULE,
	.name = FLUX_HOSTFS_NAME,
	.mount = flux_hostfs_mount,
	.kill_sb = flux_hostfs_kill_sb,
	.fs_flags = 0,
};

static int __init flux_hostfs_init(void)
{
	return register_filesystem(&flux_hostfs_type);
}
fs_initcall(flux_hostfs_init);

static void __exit flux_hostfs_exit(void)
{
	unregister_filesystem(&flux_hostfs_type);
}
module_exit(flux_hostfs_exit);
MODULE_LICENSE("GPL");
