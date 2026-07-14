#include <dirent.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <flux.h>

#define MAX_FSTYPE_LEN 50
int flux_mount_fs(char *fstype)
{
	char dir[MAX_FSTYPE_LEN + 2] = "/";
	int flags = 0, ret = 0;

	strncat(dir, fstype, MAX_FSTYPE_LEN);

	/* Create with regular umask */
	ret = flux_sys_mkdir(dir, 0xff);
	if (ret && ret != -FLUX_EEXIST) {
		flux_perror("mount_fs mkdir", ret);
		return ret;
	}

	/* We have no use for nonzero flags right now */
	ret = flux_sys_mount("none", dir, fstype, flags, NULL);
	if (ret && ret != -FLUX_EBUSY) {
		flux_sys_rmdir(dir);
		return ret;
	}

	if (ret == -FLUX_EBUSY)
		return 1;
	return 0;
}

int flux_mkdir_p(const char *path, flux_mode_t mode)
{
	char tmp[FLUX_PATH_MAX];
	size_t len;
	int err;

	if (!path || path[0] != '/')
		return -FLUX_EINVAL;

	if (!strcmp(path, "/"))
		return 0;

	len = strlen(path);
	if (len >= sizeof(tmp))
		return -FLUX_ENAMETOOLONG;

	memcpy(tmp, path, len + 1);
	for (size_t i = 1; i < len; i++) {
		if (tmp[i] != '/')
			continue;

		tmp[i] = '\0';
		err = flux_sys_mkdir(tmp, 0755);
		if (err < 0 && err != -FLUX_EEXIST)
			return err;
		tmp[i] = '/';
	}

	err = flux_sys_mkdir(tmp, mode);
	if (err < 0 && err != -FLUX_EEXIST)
		return err;

	return 0;
}

int flux_mount_with_mkdir(const struct flux_mount_spec *mount)
{
	int err;

	if (!mount || !mount->target || !mount->type)
		return -FLUX_EINVAL;

	err = flux_mkdir_p(mount->target, mount->mode);
	if (err < 0)
		return err;

	return flux_sys_mount((char *)mount->source, (char *)mount->target,
			      (char *)mount->type, mount->flags,
			      (char *)mount->data);
}

unsigned long flux_hostfs_mount_flags(void)
{
	if (run_cfg && run_cfg->hostfs_writable && atoi(run_cfg->hostfs_writable))
		return 0;

	return FLUX_MS_RDONLY;
}

static struct flux_dir *flux_dir_alloc(int *err)
{
	struct flux_dir *dir;

	dir = flux_host_ops.mem_alloc(sizeof(struct flux_dir));

	if (!dir) {
		*err = -FLUX_ENOMEM;
		return NULL;
	}

	dir->len = 0;
	dir->pos = NULL;

	return dir;
}

struct flux_dir *flux_opendir(const char *path, int *err)
{
	struct flux_dir *dir = flux_dir_alloc(err);

	if (!dir) {
		*err = -FLUX_ENOMEM;
		return NULL;
	}

	dir->fd = flux_sys_open(path, FLUX_O_RDONLY | FLUX_O_DIRECTORY, 0);
	if (dir->fd < 0) {
		*err = dir->fd;
		flux_host_ops.mem_free(dir);
		return NULL;
	}

	*err = 0;

	return dir;
}

struct flux_dir *flux_fdopendir(int fd, int *err)
{
	struct flux_dir *dir = flux_dir_alloc(err);

	if (!dir)
		return NULL;

	dir->fd = fd;

	return dir;
}

void flux_rewinddir(struct flux_dir *dir)
{
	flux_sys_lseek(dir->fd, 0, FLUX_SEEK_SET);
	dir->len = 0;
	dir->pos = NULL;
}

int flux_closedir(struct flux_dir *dir)
{
	int ret;

	ret = flux_sys_close(dir->fd);
	flux_host_ops.mem_free(dir);

	return ret;
}

struct flux_linux_dirent64 *flux_readdir(struct flux_dir *dir)
{
	struct flux_linux_dirent64 *de;

	if (dir->len < 0)
		return NULL;

	if (!dir->pos || dir->pos - dir->buf >= dir->len)
		goto read_buf;

return_de:
	de = (struct flux_linux_dirent64 *)dir->pos;
	dir->pos += de->d_reclen;

	return de;

read_buf:
	dir->pos = NULL;
	de = (struct flux_linux_dirent64 *)dir->buf;
	dir->len = flux_sys_getdents64(dir->fd, de, sizeof(dir->buf));
	if (dir->len <= 0)
		return NULL;

	dir->pos = dir->buf;
	goto return_de;
}

int flux_removedir(const char *path)
{
	int ret;
	struct flux_linux_dirent64 *entry;
	struct flux_dir *dir;
	char subpath[FLUX_PATH_MAX];

	dir = flux_opendir(path, &ret);
	if (!dir)
		return ret;

	while ((entry = flux_readdir(dir)) != NULL) {
		snprintf(subpath, sizeof(subpath), "%s/%s", path,
			 entry->d_name);

		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;

		if (entry->d_type == DT_DIR) {
			if ((ret = flux_removedir(subpath)) < 0)
				return ret;

			if ((ret = flux_sys_rmdir(subpath)) < 0)
				return ret;
		} else {
			if ((ret = flux_sys_unlink(path)) < 0)
				return ret;
		}
	}

	if ((ret = flux_closedir(dir)) < 0)
		return ret;

	return 0;
}

int flux_errdir(struct flux_dir *dir)
{
	if (dir->len >= 0)
		return 0;

	return dir->len;
}

int flux_dirfd(struct flux_dir *dir)
{
	return dir->fd;
}

int flux_set_fd_limit(unsigned int fd_limit)
{
	struct flux_rlimit rlim = {
		.rlim_cur = fd_limit,
		.rlim_max = fd_limit,
	};
	return flux_sys_setrlimit(FLUX_RLIMIT_NOFILE, &rlim);
}
