#ifndef _UTILS_FS_H
#define _UTILS_FS_H

#include <flux/base.h>
#include <utils/base.h>

/*
 * Directory iteration state used by Flux's userspace directory helpers.
 *
 * The backing buffer stores raw getdents64 output and is intentionally opaque
 * to callers.
 */
struct flux_dir {
	int fd;
	char buf[1024];
	char *pos;
	int len;
};

struct flux_mount_spec {
	const char *source;
	const char *target;
	const char *type;
	const char *data;
	flux_mode_t mode;
	unsigned long flags;
};

/* Open a directory by path and return a reusable Flux directory iterator. */
struct flux_dir *flux_opendir(const char *path, int *err);

/* Adopt an existing fd as a Flux directory iterator. */
struct flux_dir *flux_fdopendir(int fd, int *err);

/* Reset iteration so the next flux_readdir() returns the first entry again. */
void flux_rewinddir(struct flux_dir *dir);

/* Close a directory iterator and release its host-side resources. */
int flux_closedir(struct flux_dir *dir);

/*
 * Read the next directory entry.
 *
 * Returns NULL on end-of-stream or error. Call flux_errdir() to tell those
 * cases apart.
 */
struct flux_linux_dirent64 *flux_readdir(struct flux_dir *dir);

/* Remove a directory tree recursively. */
int flux_removedir(const char *path);

/* Retrieve the error recorded by the last flux_readdir() call. */
int flux_errdir(struct flux_dir *dir);

/* Return the underlying Flux file descriptor backing the iterator. */
int flux_dirfd(struct flux_dir *dir);

/*
 * Mount a pseudo filesystem such as proc or sys.
 *
 * Returns 0 on success, 1 if the mount already exists, or a negative error.
 */
int flux_mount_fs(char *fstype);

/* Recursively create a directory path inside the guest filesystem. */
int flux_mkdir_p(const char *path, flux_mode_t mode);

/* Ensure the target exists, then perform the mount. */
int flux_mount_with_mkdir(const struct flux_mount_spec *mount);

/* Shared default for hostfs mounts: read-only unless explicitly enabled. */
unsigned long flux_hostfs_mount_flags(void);

#endif /* _UTILS_FS_H */
