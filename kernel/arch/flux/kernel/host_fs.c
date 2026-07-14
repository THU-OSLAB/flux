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
#include <linux/pagemap.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>

#define FLUX_HOSTFS_NAME "flux_hostfs"
#define FLUX_HOSTFS_DIRBUF_SIZE 4096

static const struct inode_operations flux_hostfs_file_iops;
static const struct inode_operations flux_hostfs_dir_iops;
static const struct inode_operations flux_hostfs_symlink_iops;
static const struct file_operations flux_hostfs_file_fops;
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
	const char *root = sb->s_fs_info;
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

static struct inode *flux_hostfs_inode_from_statx(struct super_block *sb,
						  const struct statx *stx)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_ino = stx->stx_ino ? stx->stx_ino : get_next_ino();
	inode->i_mode = stx->stx_mode;
	inode->i_uid = make_kuid(&init_user_ns, stx->stx_uid);
	inode->i_gid = make_kgid(&init_user_ns, stx->stx_gid);
	inode->i_size = stx->stx_size;
	inode->i_blocks = stx->stx_blocks;
	inode->i_atime = (struct timespec64){ stx->stx_atime.tv_sec,
					      stx->stx_atime.tv_nsec };
	inode->i_mtime = (struct timespec64){ stx->stx_mtime.tv_sec,
					      stx->stx_mtime.tv_nsec };
	inode_set_ctime(inode, stx->stx_ctime.tv_sec, stx->stx_ctime.tv_nsec);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &flux_hostfs_dir_iops;
		inode->i_fop = &flux_hostfs_dir_fops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &flux_hostfs_symlink_iops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &flux_hostfs_file_iops;
		inode->i_fop = &flux_hostfs_file_fops;
	} else {
		init_special_inode(inode, inode->i_mode,
				   MKDEV(stx->stx_rdev_major,
					 stx->stx_rdev_minor));
		inode->i_op = &flux_hostfs_file_iops;
	}

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
		err = host_syscall(__NR_truncate, path, attr->ia_size);
		if (err)
			goto out;
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

	file->private_data = (void *)(long)fd;
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
	if (!filp) {
		ret = -EBADF;
		goto out;
	}

	ret = flux_hostfs_get_host_fd(filp);

out:
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

static ssize_t flux_hostfs_read(struct file *file, char __user *buf, size_t len,
				loff_t *ppos)
{
	long fd = (long)file->private_data;
	long ret;

	if (fd < 0)
		return -EBADF;

	ret = host_syscall(__NR_pread64, fd, buf, len, *ppos);
	if (ret > 0)
		*ppos += ret;
	return ret;
}

static ssize_t flux_hostfs_write(struct file *file, const char __user *buf,
				 size_t len, loff_t *ppos)
{
	long fd = (long)file->private_data;
	long ret;

	if (fd < 0)
		return -EBADF;

	ret = host_syscall(__NR_pwrite64, fd, buf, len, *ppos);
	if (ret > 0)
		*ppos += ret;
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
		if (err == -ENOENT)
			return NULL;
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

static const struct file_operations flux_hostfs_file_fops = {
	.llseek = generic_file_llseek,
	.read = flux_hostfs_read,
	.write = flux_hostfs_write,
	.open = flux_hostfs_open,
	.release = flux_hostfs_release,
};

static const struct file_operations flux_hostfs_dir_fops = {
	.llseek = generic_file_llseek,
	.iterate_shared = flux_hostfs_readdir,
	.read = generic_read_dir,
	.open = flux_hostfs_open,
	.release = flux_hostfs_release,
};

static int flux_hostfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct statfs st;
	const char *root = dentry->d_sb->s_fs_info;
	long err;

	err = host_syscall(__NR_statfs, root, &st);
	if (err)
		return err;

	buf->f_type = HOSTFS_SUPER_MAGIC;
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
}

static const struct super_operations flux_hostfs_sb_ops = {
	.statfs = flux_hostfs_statfs,
	.drop_inode = generic_delete_inode,
	.evict_inode = flux_hostfs_evict_inode,
};

static int flux_hostfs_fill_super(struct super_block *sb, void *data,
				  int silent)
{
	struct inode *inode;
	struct statx stx;
	const char *dev_name = data;
	char *root;
	int err;

	root = flux_hostfs_sanitize_root(dev_name);
	if (!root)
		return -ENOMEM;

	sb->s_magic = HOSTFS_SUPER_MAGIC;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &flux_hostfs_sb_ops;
	sb->s_fs_info = root;

	err = flux_hostfs_statx(root, &stx, 0);
	if (err) {
		kfree(root);
		return err;
	}

	inode = flux_hostfs_inode_from_statx(sb, &stx);
	if (IS_ERR(inode)) {
		kfree(root);
		return PTR_ERR(inode);
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		kfree(root);
		return -ENOMEM;
	}

	return 0;
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
	kfree(sb->s_fs_info);
	kill_anon_super(sb);
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
