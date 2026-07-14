#define FLUX_FMT "oci: "

#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <flux.h>
#include <flux/runc.h>

#define FLUX_OCI_CONTAINER_ROOT "/.flux_oci_root"

struct flux_oci_dev_node {
	const char *path;
	flux_mode_t mode;
	unsigned int dev;
};

struct flux_oci_symlink {
	const char *path;
	const char *target;
};

static bool flux_oci_path_matches_prefix(const char *path, const char *prefix)
{
	size_t len;

	if (!path || !prefix)
		return false;

	len = strlen(prefix);
	return strncmp(path, prefix, len) == 0 &&
	       (path[len] == '\0' || path[len] == '/');
}

static int flux_oci_container_root_path(const char *container_path, char *buf,
					size_t buf_sz)
{
	if (!container_path || container_path[0] != '/')
		return -FLUX_EINVAL;

	if (!strcmp(container_path, "/")) {
		if (snprintf(buf, buf_sz, "%s", FLUX_OCI_CONTAINER_ROOT) >=
		    (int)buf_sz)
			return -FLUX_ENAMETOOLONG;
		return 0;
	}

	if (snprintf(buf, buf_sz, "%s%s", FLUX_OCI_CONTAINER_ROOT,
		     container_path) >= (int)buf_sz)
		return -FLUX_ENAMETOOLONG;

	return 0;
}

static int flux_oci_rootfs_host_path(const char *container_path, char *buf,
				     size_t buf_sz)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();

	if (!oci || !oci->rootfs_path || !container_path ||
	    container_path[0] != '/')
		return -FLUX_EINVAL;

	if (!strcmp(container_path, "/")) {
		if (snprintf(buf, buf_sz, "%s", oci->rootfs_path) >=
		    (int)buf_sz)
			return -FLUX_ENAMETOOLONG;
		return 0;
	}

	if (snprintf(buf, buf_sz, "%s%s", oci->rootfs_path, container_path) >=
	    (int)buf_sz)
		return -FLUX_ENAMETOOLONG;

	return 0;
}

static int flux_oci_host_mkdir_p(const char *path, mode_t mode)
{
	char tmp[FLUX_PATH_MAX];
	size_t len;

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
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -errno;
		tmp[i] = '/';
	}

	if (mkdir(tmp, mode) < 0 && errno != EEXIST)
		return -errno;

	return 0;
}

static int
flux_oci_mount_into_container_root(const struct flux_mount_spec *entry)
{
	char mount_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_path(entry->target, mount_path,
					   sizeof(mount_path));
	if (err < 0)
		return err;

	return flux_sys_mount((char *)entry->source, mount_path,
			      (char *)entry->type, entry->flags,
			      (char *)entry->data);
}

static int flux_oci_container_root_path_exists(const char *container_path)
{
	char root_path[FLUX_PATH_MAX];
	struct flux_stat st;
	int err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	err = flux_sys_stat(root_path, &st);
	if (err == 0)
		return 1;
	if (err == -FLUX_ENOENT)
		return 0;

	return err;
}

static bool flux_oci_has_mount_destination(const char *destination)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	int i;

	if (!oci || !destination)
		return false;

	for (i = 0; i < oci->mount_num; i++) {
		if (!strcmp(oci->mounts[i].destination, destination))
			return true;
	}

	return false;
}

static int flux_oci_find_host_third_party_from(const char *start_dir,
					       const char *container_path,
					       char *source, size_t source_sz)
{
	char dir[FLUX_PATH_MAX];
	struct stat st;

	if (!start_dir || start_dir[0] != '/' || !container_path ||
	    !flux_oci_path_matches_prefix(container_path, "/third-party"))
		return -FLUX_EINVAL;

	if (strlen(start_dir) >= sizeof(dir))
		return -FLUX_ENAMETOOLONG;

	strcpy(dir, start_dir);
	while (true) {
		char candidate[FLUX_PATH_MAX];

		if (snprintf(candidate, sizeof(candidate), "%s%s", dir,
			     container_path) >= (int)sizeof(candidate))
			return -FLUX_ENAMETOOLONG;

		if (stat(candidate, &st) == 0) {
			if (snprintf(source, source_sz, "%s/third-party",
				     dir) >= (int)source_sz)
				return -FLUX_ENAMETOOLONG;
			return 0;
		}

		if (!strcmp(dir, "/"))
			break;

		char *slash = strrchr(dir, '/');
		if (!slash)
			break;
		if (slash == dir)
			dir[1] = '\0';
		else
			*slash = '\0';
	}

	return -FLUX_ENOENT;
}

static int flux_oci_find_host_third_party(const char *container_path,
					  char *source, size_t source_sz)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	char exe[FLUX_PATH_MAX];
	char cwd[FLUX_PATH_MAX];
	ssize_t len;
	int err;

	if (!oci || !container_path || !source)
		return -FLUX_EINVAL;

	err = flux_oci_find_host_third_party_from(
		oci->bundle_dir, container_path, source, source_sz);
	if (err == 0 || err != -FLUX_ENOENT)
		return err;

	err = flux_oci_find_host_third_party_from(
		oci->rootfs_path, container_path, source, source_sz);
	if (err == 0 || err != -FLUX_ENOENT)
		return err;

	if (!getcwd(cwd, sizeof(cwd)))
		return -errno;

	err = flux_oci_find_host_third_party_from(cwd, container_path, source,
						  source_sz);
	if (err == 0 || err != -FLUX_ENOENT)
		return err;

	len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len < 0)
		return -errno;
	exe[len] = '\0';

	{
		char *slash = strrchr(exe, '/');

		if (!slash)
			return -FLUX_ENOENT;
		if (slash == exe)
			exe[1] = '\0';
		else
			*slash = '\0';
	}

	return flux_oci_find_host_third_party_from(exe, container_path, source,
						   source_sz);
}

static int flux_oci_host_prepare_parent(const char *path)
{
	char dir[FLUX_PATH_MAX];
	char *slash;

	if (!path || path[0] != '/')
		return -FLUX_EINVAL;

	if (strlen(path) >= sizeof(dir))
		return -FLUX_ENAMETOOLONG;

	strcpy(dir, path);
	slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;

	*slash = '\0';
	return flux_oci_host_mkdir_p(dir, 0755);
}

static int flux_oci_host_touch(const char *path, mode_t mode)
{
	int fd;

	fd = open(path, O_CREAT | O_CLOEXEC, mode);
	if (fd < 0)
		return -errno;

	if (close(fd) < 0)
		return -errno;

	return 0;
}

static int
flux_oci_prepare_host_mount_target(const struct flux_oci_mount *mount)
{
	char rootfs_host_path[FLUX_PATH_MAX];
	struct stat st;
	int err;

	err = flux_oci_rootfs_host_path(mount->destination, rootfs_host_path,
					sizeof(rootfs_host_path));
	if (err < 0)
		return err;

	if (!mount->use_hostfs || !mount->source)
		return flux_oci_host_mkdir_p(rootfs_host_path, 0755);

	if (stat(mount->source, &st) < 0)
		return -errno;

	if (S_ISDIR(st.st_mode))
		return flux_oci_host_mkdir_p(rootfs_host_path, 0755);

	err = flux_oci_host_prepare_parent(rootfs_host_path);
	if (err < 0)
		return err;

	return flux_oci_host_touch(rootfs_host_path, 0644);
}

static int flux_oci_mount_target_is_dir(const struct flux_oci_mount *mount,
					bool *is_dir)
{
	struct stat st;

	if (!mount || !is_dir)
		return -FLUX_EINVAL;

	if (!mount->use_hostfs || !mount->source) {
		*is_dir = true;
		return 0;
	}

	if (stat(mount->source, &st) < 0)
		return -errno;

	*is_dir = S_ISDIR(st.st_mode);
	return 0;
}

static int flux_oci_prepare_container_root_parent(const char *container_path)
{
	char root_path[FLUX_PATH_MAX];
	char *slash;
	int err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	slash = strrchr(root_path, '/');
	if (!slash || slash == root_path)
		return 0;

	*slash = '\0';
	return flux_mkdir_p(root_path, 0755);
}

static int
flux_oci_prepare_container_root_mount_target(const struct flux_oci_mount *mount)
{
	char root_path[FLUX_PATH_MAX];
	struct flux_stat st;
	bool is_dir = false;
	int fd;
	int err;

	err = flux_oci_container_root_path(mount->destination, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	err = flux_sys_stat(root_path, &st);
	if (err == 0)
		return 0;
	if (err != -FLUX_ENOENT)
		return err;

	err = flux_oci_mount_target_is_dir(mount, &is_dir);
	if (err < 0)
		return err;

	if (is_dir)
		return flux_mkdir_p(root_path, 0755);

	err = flux_oci_prepare_container_root_parent(mount->destination);
	if (err < 0)
		return err;

	fd = flux_sys_open(root_path, FLUX_O_CREAT | FLUX_O_RDONLY, 0644);
	if (fd < 0)
		return fd;

	return flux_sys_close(fd);
}

static int flux_oci_maybe_mount_run_cfg_third_party(void)
{
	char source[FLUX_PATH_MAX];
	struct flux_oci_mount mount = {
		.destination = "/third-party",
		.type = "none",
		.use_hostfs = true,
	};
	struct flux_mount_spec entry = {
		.target = "/third-party",
		.type = "flux_hostfs",
		.mode = 0755,
		.flags = flux_hostfs_mount_flags(),
	};
	int err;
	int exists;

	if (!run_cfg || !run_cfg->ld_path || !run_cfg->ld_path[0] ||
	    !flux_oci_path_matches_prefix(run_cfg->ld_path, "/third-party"))
		return 0;

	if (flux_oci_has_mount_destination("/third-party"))
		return 0;

	exists = flux_oci_container_root_path_exists(run_cfg->ld_path);
	if (exists < 0)
		return exists;
	if (exists > 0)
		return 0;

	err = flux_oci_find_host_third_party(run_cfg->ld_path, source,
					     sizeof(source));
	if (err == -FLUX_ENOENT) {
		FLUX_LOG(
			FLUX_LOG_INFO,
			"no host third-party found for configured interp %s; leaving OCI bundle mounts unchanged\n",
			run_cfg->ld_path);
		return 0;
	}
	if (err < 0)
		return err;

	mount.source = source;
	entry.source = source;

	err = flux_oci_prepare_host_mount_target(&mount);
	if (err < 0)
		return err;

	err = flux_oci_prepare_container_root_mount_target(&mount);
	if (err < 0)
		return err;

	err = flux_oci_mount_into_container_root(&entry);
	if (err < 0) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"failed to auto-mount host third-party %s at /third-party: %s\n",
			source, flux_strerror(err));
		return err;
	}

	FLUX_LOG(
		FLUX_LOG_INFO,
		"auto-mounted host third-party at /third-party for configured interp\n");
	return 0;
}

static int flux_oci_mknod_ignore_exists(const char *container_path,
					flux_mode_t mode, unsigned int dev)
{
	char root_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	err = flux_sys_mknod(root_path, mode, dev);
	if (err < 0 && err != -FLUX_EEXIST)
		return err;

	return 0;
}

static int flux_oci_ensure_container_cwd(void)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	char rootfs_host_path[FLUX_PATH_MAX];
	int err;

	if (!oci || !oci->cwd || !strcmp(oci->cwd, "/"))
		return 0;

	err = flux_oci_rootfs_host_path(oci->cwd, rootfs_host_path,
					sizeof(rootfs_host_path));
	if (err < 0)
		return err;

	return flux_oci_host_mkdir_p(rootfs_host_path, 0755);
}

static int flux_oci_container_root_stat(const char *container_path,
					struct flux_stat *st)
{
	char root_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	return flux_sys_stat(root_path, st);
}

static int flux_oci_bind_container_root_path(const char *source,
					     const char *container_path,
					     unsigned long flags)
{
	char root_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	return flux_sys_mount((char *)source, root_path, NULL, flags, NULL);
}

static int flux_oci_apply_masked_path(const char *container_path)
{
	struct flux_stat st;
	char null_root_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_stat(container_path, &st);
	if (err == -FLUX_ENOENT)
		return 0;
	if (err < 0)
		return err;

	if (FLUX_S_ISDIR(st.st_mode)) {
		return flux_oci_mount_into_container_root(
			&(struct flux_mount_spec){
				.source = "tmpfs",
				.target = container_path,
				.type = "tmpfs",
				.data = "mode=000",
				.mode = 0000,
				.flags = FLUX_MS_RDONLY,
			});
	}

	err = flux_oci_container_root_path("/dev/null", null_root_path,
					   sizeof(null_root_path));
	if (err < 0)
		return err;

	return flux_oci_bind_container_root_path(null_root_path, container_path,
						 FLUX_MS_BIND);
}

static int flux_oci_apply_readonly_path(const char *container_path)
{
	struct flux_stat st;
	char root_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_stat(container_path, &st);
	if (err == -FLUX_ENOENT)
		return 0;
	if (err < 0)
		return err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	err = flux_sys_mount(root_path, root_path, NULL,
			     FLUX_MS_BIND | FLUX_MS_REC, NULL);
	if (err < 0)
		return err;

	return flux_sys_mount(NULL, root_path, NULL,
			      FLUX_MS_BIND | FLUX_MS_REMOUNT | FLUX_MS_RDONLY,
			      NULL);
}

static int flux_oci_apply_linux_path_policies(void)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	int err;
	int i;

	if (!oci)
		return 0;

	for (i = 0; i < oci->masked_paths_num; i++) {
		err = flux_oci_apply_masked_path(oci->masked_paths[i]);
		if (err < 0)
			return err;
	}

	for (i = 0; i < oci->readonly_paths_num; i++) {
		err = flux_oci_apply_readonly_path(oci->readonly_paths[i]);
		if (err < 0)
			return err;
	}

	return 0;
}

static int flux_oci_symlink_ignore_exists(const char *target,
					  const char *container_path)
{
	char root_path[FLUX_PATH_MAX];
	int err;

	err = flux_oci_container_root_path(container_path, root_path,
					   sizeof(root_path));
	if (err < 0)
		return err;

	err = flux_sys_symlink(target, root_path);
	if (err < 0 && err != -FLUX_EEXIST)
		return err;

	return 0;
}

static int flux_oci_populate_devtmpfs(void)
{
	static const struct flux_oci_dev_node nodes[] = {
		{ "/dev/null", FLUX_S_IFCHR | 0666, FLUX_MKDEV(1, 3) },
		{ "/dev/zero", FLUX_S_IFCHR | 0666, FLUX_MKDEV(1, 5) },
		{ "/dev/full", FLUX_S_IFCHR | 0666, FLUX_MKDEV(1, 7) },
		{ "/dev/random", FLUX_S_IFCHR | 0666, FLUX_MKDEV(1, 8) },
		{ "/dev/urandom", FLUX_S_IFCHR | 0666, FLUX_MKDEV(1, 9) },
		{ "/dev/tty", FLUX_S_IFCHR | 0666, FLUX_MKDEV(5, 0) },
		{ "/dev/console", FLUX_S_IFCHR | 0600, FLUX_MKDEV(5, 1) },
	};
	static const struct flux_oci_symlink symlinks[] = {
		{ "/dev/fd", "/proc/self/fd" },
		{ "/dev/stdin", "/proc/self/fd/0" },
		{ "/dev/stdout", "/proc/self/fd/1" },
		{ "/dev/stderr", "/proc/self/fd/2" },
		{ "/dev/ptmx", "pts/ptmx" },
	};
	size_t i;
	int err;

	err = flux_mkdir_p(FLUX_OCI_CONTAINER_ROOT "/dev/pts", 0755);
	if (err < 0)
		return err;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		err = flux_oci_mknod_ignore_exists(nodes[i].path, nodes[i].mode,
						   nodes[i].dev);
		if (err < 0)
			return err;
	}

	for (i = 0; i < sizeof(symlinks) / sizeof(symlinks[0]); i++) {
		err = flux_oci_symlink_ignore_exists(symlinks[i].target,
						     symlinks[i].path);
		if (err < 0)
			return err;
	}

	return 0;
}

int flux_runc_enter_container_fs(void)
{
	int err;

	err = flux_sys_chdir(FLUX_OCI_CONTAINER_ROOT);
	if (err < 0)
		return err;

	err = flux_sys_chroot(".");
	if (err < 0)
		return err;

	return flux_sys_chdir("/");
}

int flux_runc_prepare_container_fs(void)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	int err;
	int i;
	bool mounted_dev_tmpfs = false;

	if (!oci)
		return -FLUX_EINVAL;

	err = flux_mkdir_p(FLUX_OCI_CONTAINER_ROOT, 0755);
	if (err < 0)
		return err;

	err = flux_oci_mount_into_container_root(&(struct flux_mount_spec){
		.source = oci->rootfs_path,
		.target = "/",
		.type = "flux_hostfs",
		.data = NULL,
		.mode = 0755,
		.flags = oci->rootfs_readonly ? FLUX_MS_RDONLY : 0,
	});
	if (err < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mount OCI rootfs %s: %s\n",
			 oci->rootfs_path, flux_strerror(err));
		return err;
	}

	err = flux_oci_maybe_mount_run_cfg_third_party();
	if (err < 0)
		return err;

	for (i = 0; i < oci->mount_num; i++) {
		const struct flux_oci_mount *mount = &oci->mounts[i];
		struct flux_mount_spec entry = {
			.source = mount->source ?
					  mount->source :
					  (mount->type ? mount->type : "none"),
			.target = mount->destination,
			.type = mount->use_hostfs ? "flux_hostfs" : mount->type,
			.data = mount->data,
			.mode = 0755,
			.flags = mount->flags,
		};

		if (!strcmp(mount->type, "mqueue")) {
			entry.source = "/dev/mqueue";
			entry.type = "flux_hostfs";
		} else if (!strcmp(mount->type, "cgroup")) {
			FLUX_LOG(
				FLUX_LOG_INFO,
				"skipping OCI cgroup mount at %s during container-fs setup\n",
				mount->destination);
			continue;
		}

		err = flux_oci_prepare_host_mount_target(mount);
		if (err < 0) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"failed to prepare OCI host mount target %s: %s\n",
				mount->destination, flux_strerror(err));
			return err;
		}

		err = flux_oci_prepare_container_root_mount_target(mount);
		if (err < 0) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"failed to prepare OCI container root mount target %s: %s\n",
				mount->destination, flux_strerror(err));
			return err;
		}

		err = flux_oci_mount_into_container_root(&entry);
		if (err < 0) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"failed to mount OCI entry %s type=%s source=%s flags=%lu data=%s: %s\n",
				mount->destination, entry.type, entry.source,
				entry.flags, entry.data ?: "(null)",
				flux_strerror(err));
			return err;
		}

		if (!strcmp(mount->destination, "/dev") &&
		    !strcmp(mount->type, "tmpfs"))
			mounted_dev_tmpfs = true;
	}

	if (mounted_dev_tmpfs) {
		err = flux_oci_populate_devtmpfs();
		if (err < 0)
			return err;
	}

	err = flux_oci_ensure_container_cwd();
	if (err < 0)
		return err;

	err = flux_oci_apply_linux_path_policies();
	if (err < 0)
		return err;

	return 0;
}
