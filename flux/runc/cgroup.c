#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <net/if.h>
#include <unistd.h>

#include "runc.h"

#define FLUX_RUNC_CGROUP_ROOT_ENV "FLUX_RUNC_CGROUP_ROOT"
#define FLUX_RUNC_CGROUP_PREFIX_ENV "FLUX_RUNC_CGROUP_PREFIX"
#define FLUX_RUNC_CGROUP_ROOT_DEFAULT "/sys/fs/cgroup"

struct flux_runc_cgroup_v1_controller {
	const char *name;
	bool cpuset;
};

static const struct flux_runc_cgroup_v1_controller
	flux_runc_cgroup_v1_controllers[] = {
		{ "blkio", false },
		{ "cpu,cpuacct", false },
		{ "cpuset", true },
		{ "devices", false },
		{ "dmem", false },
		{ "freezer", false },
		{ "hugetlb", false },
		{ "memory", false },
		{ "misc", false },
		{ "net_cls,net_prio", false },
		{ "perf_event", false },
		{ "pids", false },
		{ "rdma", false },
		{ "systemd", false },
	};

static int flux_runc_cgroup_root(char *buf, size_t size)
{
	const char *root = getenv(FLUX_RUNC_CGROUP_ROOT_ENV);

	if (!root || !root[0])
		root = FLUX_RUNC_CGROUP_ROOT_DEFAULT;
	if (root[0] != '/' || root[strlen(root) - 1] == '/')
		return -EINVAL;
	if (snprintf(buf, size, "%s", root) >= (int)size)
		return -ENAMETOOLONG;
	return 0;
}

static bool flux_runc_cgroup_component_valid(const char *value)
{
	const unsigned char *p = (const unsigned char *)value;

	if (!value || !value[0] || !strcmp(value, ".") ||
	    !strcmp(value, ".."))
		return false;
	for (; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
		    *p == '.')
			continue;
		return false;
	}
	return true;
}

static int flux_runc_cgroup_paths(const struct flux_runc_state *state,
				  char *prefix_path, size_t prefix_size,
				  char *leaf_path, size_t leaf_size)
{
	char root[PATH_MAX];
	const char *prefix = getenv(FLUX_RUNC_CGROUP_PREFIX_ENV);
	const char *leaf;
	size_t prefix_len;

	if (!state || !state->cgroup_path || !state->cgroup_path[0])
		return 1;
	if (!prefix || prefix[0] != '/' || !prefix[1] ||
	    prefix[strlen(prefix) - 1] == '/')
		return -EINVAL;
	if (flux_runc_cgroup_root(root, sizeof(root)) < 0)
		return -EINVAL;

	prefix_len = strlen(prefix);
	if (strncmp(state->cgroup_path, prefix, prefix_len) ||
	    state->cgroup_path[prefix_len] != '/' ||
	    !state->cgroup_path[prefix_len + 1])
		return -EPERM;

	leaf = state->cgroup_path + prefix_len + 1;
	if (strchr(leaf, '/') || !flux_runc_cgroup_component_valid(leaf) ||
	    (state->id && strcmp(leaf, state->id)))
		return -EPERM;

	if (snprintf(prefix_path, prefix_size, "%s%s", root, prefix) >=
	    (int)prefix_size)
		return -ENAMETOOLONG;
	if (snprintf(leaf_path, leaf_size, "%s%s", root,
		     state->cgroup_path) >= (int)leaf_size)
		return -ENAMETOOLONG;

	return 0;
}

static int flux_runc_cgroup_check_parent(const char *prefix_path)
{
	char controllers[PATH_MAX];
	struct stat st;

	if (lstat(prefix_path, &st) < 0)
		return -errno;
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
		return -ENOTDIR;
	if (snprintf(controllers, sizeof(controllers), "%s/cgroup.controllers",
		     prefix_path) >= (int)sizeof(controllers))
		return -ENAMETOOLONG;
	if (access(controllers, R_OK) < 0)
		return -errno;
	return 0;
}

static bool flux_runc_cgroup_v2(const char *root)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/cgroup.controllers", root) >=
	    (int)sizeof(path))
		return false;
	return access(path, R_OK) == 0;
}

static bool flux_runc_cgroup_v1_controller_active(
	const char *root, const struct flux_runc_cgroup_v1_controller *controller)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s/tasks", root,
		     controller->name) >= (int)sizeof(path))
		return false;
	return access(path, F_OK) == 0;
}

static int flux_runc_cgroup_v1_paths(
	const char *root, const struct flux_runc_cgroup_v1_controller *controller,
	const struct flux_runc_state *state, char *prefix_path,
	size_t prefix_size, char *leaf_path, size_t leaf_size)
{
	const char *prefix = getenv(FLUX_RUNC_CGROUP_PREFIX_ENV);

	if (!prefix)
		return -EINVAL;
	if (snprintf(prefix_path, prefix_size, "%s/%s%s", root,
		     controller->name, prefix) >= (int)prefix_size)
		return -ENAMETOOLONG;
	if (snprintf(leaf_path, leaf_size, "%s/%s%s", root,
		     controller->name, state->cgroup_path) >= (int)leaf_size)
		return -ENAMETOOLONG;
	return 0;
}

static int flux_runc_cgroup_copy_value(const char *from_dir,
				       const char *to_dir, const char *name)
{
	char from[PATH_MAX];
	char to[PATH_MAX];
	char value[4096];
	ssize_t nr;
	ssize_t nw;
	int in_fd;
	int out_fd;
	int ret = 0;

	if (snprintf(from, sizeof(from), "%s/%s", from_dir, name) >=
		    (int)sizeof(from) ||
	    snprintf(to, sizeof(to), "%s/%s", to_dir, name) >=
		    (int)sizeof(to))
		return -ENAMETOOLONG;
	in_fd = open(from, O_RDONLY | O_CLOEXEC);
	if (in_fd < 0)
		return -errno;
	nr = read(in_fd, value, sizeof(value));
	if (nr < 0)
		ret = -errno;
	close(in_fd);
	if (ret < 0)
		return ret;

	out_fd = open(to, O_WRONLY | O_CLOEXEC);
	if (out_fd < 0)
		return -errno;
	do {
		nw = write(out_fd, value, (size_t)nr);
	} while (nw < 0 && errno == EINTR);
	if (nw != nr)
		ret = nw < 0 ? -errno : -EIO;
	if (close(out_fd) < 0 && ret == 0)
		ret = -errno;
	return ret;
}

static int flux_runc_cgroup_v1_init_cpuset(const char *parent,
					   const char *child)
{
	int ret;

	ret = flux_runc_cgroup_copy_value(parent, child, "cpuset.cpus");
	if (ret < 0)
		return ret;
	return flux_runc_cgroup_copy_value(parent, child, "cpuset.mems");
}

static int flux_runc_cgroup_v1_create(const char *root,
				      const struct flux_runc_state *state)
{
	bool created[sizeof(flux_runc_cgroup_v1_controllers) /
		     sizeof(flux_runc_cgroup_v1_controllers[0])] = { 0 };
	size_t i;
	int ret = 0;

	for (i = 0; i < sizeof(flux_runc_cgroup_v1_controllers) /
			       sizeof(flux_runc_cgroup_v1_controllers[0]);
	     i++) {
		const struct flux_runc_cgroup_v1_controller *controller =
			&flux_runc_cgroup_v1_controllers[i];
		char controller_root[PATH_MAX];
		char prefix_path[PATH_MAX];
		char leaf_path[PATH_MAX];
		bool prefix_created = false;

		if (!flux_runc_cgroup_v1_controller_active(root, controller))
			continue;
		if (snprintf(controller_root, sizeof(controller_root), "%s/%s",
			     root, controller->name) >= (int)sizeof(controller_root)) {
			ret = -ENAMETOOLONG;
			break;
		}
		ret = flux_runc_cgroup_v1_paths(
			root, controller, state, prefix_path, sizeof(prefix_path),
			leaf_path, sizeof(leaf_path));
		if (ret < 0)
			break;
		if (mkdir(prefix_path, 0755) == 0) {
			prefix_created = true;
		} else if (errno != EEXIST) {
			ret = -errno;
			break;
		}
		if (prefix_created && controller->cpuset) {
			ret = flux_runc_cgroup_v1_init_cpuset(controller_root,
							  prefix_path);
			if (ret < 0)
				break;
		}
		if (mkdir(leaf_path, 0755) < 0) {
			ret = errno == EEXIST ? -EEXIST : -errno;
			break;
		}
		created[i] = true;
		if (controller->cpuset) {
			ret = flux_runc_cgroup_v1_init_cpuset(prefix_path,
							  leaf_path);
			if (ret < 0)
				break;
		}
	}

	if (ret < 0) {
		for (i = sizeof(flux_runc_cgroup_v1_controllers) /
				 sizeof(flux_runc_cgroup_v1_controllers[0]);
		     i > 0; i--) {
			char prefix_path[PATH_MAX];
			char leaf_path[PATH_MAX];

			if (!created[i - 1])
				continue;
			if (flux_runc_cgroup_v1_paths(
				    root, &flux_runc_cgroup_v1_controllers[i - 1],
				    state, prefix_path, sizeof(prefix_path), leaf_path,
				    sizeof(leaf_path)) == 0)
				(void)rmdir(leaf_path);
		}
	}
	return ret;
}

static int flux_runc_cgroup_write_pid(const char *dir, const char *file,
				      pid_t pid)
{
	char path[PATH_MAX];
	char pid_buf[64];
	ssize_t len;
	ssize_t nw;
	int fd;
	int ret;

	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;
	len = snprintf(pid_buf, sizeof(pid_buf), "%ld\n", (long)pid);
	if (len <= 0 || len >= (ssize_t)sizeof(pid_buf))
		return -EINVAL;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		nw = write(fd, pid_buf, (size_t)len);
	} while (nw < 0 && errno == EINTR);
	if (nw != len)
		ret = nw < 0 ? -errno : -EIO;
	else
		ret = 0;
	if (close(fd) < 0 && ret == 0)
		ret = -errno;
	return ret;
}

static int flux_runc_cgroup_v1_join(const char *root,
				    const struct flux_runc_state *state,
				    pid_t pid)
{
	size_t i;

	for (i = 0; i < sizeof(flux_runc_cgroup_v1_controllers) /
			       sizeof(flux_runc_cgroup_v1_controllers[0]);
	     i++) {
		const struct flux_runc_cgroup_v1_controller *controller =
			&flux_runc_cgroup_v1_controllers[i];
		char prefix_path[PATH_MAX];
		char leaf_path[PATH_MAX];
		int ret;

		if (!flux_runc_cgroup_v1_controller_active(root, controller))
			continue;
		ret = flux_runc_cgroup_v1_paths(
			root, controller, state, prefix_path, sizeof(prefix_path),
			leaf_path, sizeof(leaf_path));
		if (ret < 0)
			return ret;
		ret = flux_runc_cgroup_write_pid(leaf_path, "cgroup.procs", pid);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int flux_runc_cgroup_v1_destroy(const char *root,
				       const struct flux_runc_state *state)
{
	int first_error = 0;
	size_t i;

	for (i = sizeof(flux_runc_cgroup_v1_controllers) /
			 sizeof(flux_runc_cgroup_v1_controllers[0]);
	     i > 0; i--) {
		const struct flux_runc_cgroup_v1_controller *controller =
			&flux_runc_cgroup_v1_controllers[i - 1];
		char prefix_path[PATH_MAX];
		char leaf_path[PATH_MAX];
		int ret;

		if (!flux_runc_cgroup_v1_controller_active(root, controller))
			continue;
		ret = flux_runc_cgroup_v1_paths(
			root, controller, state, prefix_path, sizeof(prefix_path),
			leaf_path, sizeof(leaf_path));
		if (ret < 0) {
			if (!first_error)
				first_error = ret;
			continue;
		}
		if (rmdir(leaf_path) < 0 && errno != ENOENT && !first_error)
			first_error = -errno;
	}
	return first_error;
}

static bool flux_runc_resources_empty(
	const struct flux_oci_resources *resources)
{
	return !resources ||
	       (!resources->has_memory && !resources->has_cpu &&
		!resources->has_pids && !resources->has_block_io &&
		!resources->has_network);
}

#define FLUX_RUNC_CGROUP_TXN_MAX_FILES 64
#define FLUX_RUNC_CGROUP_TXN_MAX_VALUE (1024U * 1024U)

struct flux_runc_cgroup_txn_entry {
	char dir[PATH_MAX];
	char name[NAME_MAX];
	char *value;
};

struct flux_runc_cgroup_txn {
	struct flux_runc_cgroup_txn_entry entries[FLUX_RUNC_CGROUP_TXN_MAX_FILES];
	int count;
};

static struct flux_runc_cgroup_txn *flux_runc_cgroup_active_txn;

static int flux_runc_cgroup_read_alloc(const char *dir, const char *name,
				       char **value_out)
{
	char path[PATH_MAX];
	char *value;
	size_t capacity = 4096;
	size_t len = 0;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	value = malloc(capacity + 1);
	if (!value) {
		close(fd);
		return -ENOMEM;
	}
	for (;;) {
		ssize_t nr;

		if (len == capacity) {
			char *grown;

			if (capacity >= FLUX_RUNC_CGROUP_TXN_MAX_VALUE) {
				free(value);
				close(fd);
				return -EFBIG;
			}
			capacity *= 2;
			if (capacity > FLUX_RUNC_CGROUP_TXN_MAX_VALUE)
				capacity = FLUX_RUNC_CGROUP_TXN_MAX_VALUE;
			grown = realloc(value, capacity + 1);
			if (!grown) {
				free(value);
				close(fd);
				return -ENOMEM;
			}
			value = grown;
		}
		nr = read(fd, value + len, capacity - len);
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			free(value);
			close(fd);
			return -errno;
		}
		if (nr == 0)
			break;
		len += (size_t)nr;
	}
	if (close(fd) < 0) {
		free(value);
		return -errno;
	}
	value[len] = '\0';
	*value_out = value;
	return 0;
}

static int flux_runc_cgroup_txn_capture(const char *dir, const char *name)
{
	struct flux_runc_cgroup_txn *txn = flux_runc_cgroup_active_txn;
	struct flux_runc_cgroup_txn_entry *entry;
	int i;
	int ret;

	if (!txn)
		return 0;
	for (i = 0; i < txn->count; i++)
		if (!strcmp(txn->entries[i].dir, dir) &&
		    !strcmp(txn->entries[i].name, name))
			return 0;
	if (txn->count >= FLUX_RUNC_CGROUP_TXN_MAX_FILES)
		return -E2BIG;
	entry = &txn->entries[txn->count];
	if (snprintf(entry->dir, sizeof(entry->dir), "%s", dir) >=
		    (int)sizeof(entry->dir) ||
	    snprintf(entry->name, sizeof(entry->name), "%s", name) >=
		    (int)sizeof(entry->name))
		return -ENAMETOOLONG;
	ret = flux_runc_cgroup_read_alloc(dir, name, &entry->value);
	if (ret < 0)
		return ret;
	txn->count++;
	return 0;
}

static int flux_runc_cgroup_write_raw(const char *dir, const char *name,
				      const char *value)
{
	char path[PATH_MAX];
	ssize_t len;
	ssize_t nw;
	int fd;
	int ret = 0;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	len = (ssize_t)strlen(value);
	do {
		nw = write(fd, value, (size_t)len);
	} while (nw < 0 && errno == EINTR);
	if (nw != len)
		ret = nw < 0 ? -errno : -EIO;
	if (close(fd) < 0 && ret == 0)
		ret = -errno;
	return ret;
}

static int flux_runc_cgroup_write(const char *dir, const char *name,
				  const char *value)
{
	int ret = flux_runc_cgroup_txn_capture(dir, name);

	if (ret < 0)
		return ret;
	return flux_runc_cgroup_write_raw(dir, name, value);
}

static void flux_runc_cgroup_txn_rollback(struct flux_runc_cgroup_txn *txn)
{
	int i;

	if (!txn)
		return;
	for (i = txn->count - 1; i >= 0; i--) {
		struct flux_runc_cgroup_txn_entry *entry = &txn->entries[i];
		char *line = entry->value;

		while (line && line[0]) {
			char *next = strchr(line, '\n');
			char saved = '\0';

			if (next) {
				saved = next[1];
				next[1] = '\0';
			}
			(void)flux_runc_cgroup_write_raw(entry->dir, entry->name,
						  line);
			if (!next)
				break;
			next[1] = saved;
			line = next + 1;
		}
	}
}

static void flux_runc_cgroup_txn_fini(struct flux_runc_cgroup_txn *txn)
{
	if (!txn)
		return;
	for (int i = 0; i < txn->count; i++)
		free(txn->entries[i].value);
}

static int flux_runc_cgroup_check_writable(const char *dir, const char *name)
{
	char path[PATH_MAX];
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (close(fd) < 0)
		return -errno;
	return 0;
}

static int flux_runc_cgroup_check_block_device(int64_t major, int64_t minor);

static int flux_runc_cgroup_v2_preflight_memory(
	const char *leaf, const struct flux_oci_memory_resources *memory)
{
	int ret;

	if (memory->has_swap && memory->swap >= 0) {
		if (!memory->has_limit || memory->limit < 0 ||
		    memory->swap < memory->limit)
			return -EINVAL;
	}
	if (memory->has_swappiness ||
	    (memory->has_disable_oom_killer && memory->disable_oom_killer) ||
	    memory->has_use_hierarchy)
		return -EOPNOTSUPP;
#define CHECK_MEMORY(field, name)                                            \
	do {                                                                   \
		if (memory->has_##field &&                                         \
		    (ret = flux_runc_cgroup_check_writable(leaf, name)) < 0)      \
			return ret;                                                \
	} while (0)
	CHECK_MEMORY(limit, "memory.max");
	CHECK_MEMORY(reservation, "memory.low");
	CHECK_MEMORY(swap, "memory.swap.max");
#undef CHECK_MEMORY
	return 0;
}

static int flux_runc_cgroup_v2_preflight_cpu(
	const char *leaf, const struct flux_oci_cpu_resources *cpu)
{
	int ret;

	if (cpu->has_realtime_runtime || cpu->has_realtime_period)
		return -EOPNOTSUPP;
#define CHECK_CPU(condition, name)                                           \
	do {                                                                   \
		if ((condition) &&                                               \
		    (ret = flux_runc_cgroup_check_writable(leaf, name)) < 0)      \
			return ret;                                                \
	} while (0)
	CHECK_CPU(cpu->has_shares, "cpu.weight");
	CHECK_CPU(cpu->has_quota || cpu->has_period, "cpu.max");
	CHECK_CPU(cpu->has_burst, "cpu.max.burst");
	CHECK_CPU(cpu->has_cpus && cpu->cpus && cpu->cpus[0], "cpuset.cpus");
	CHECK_CPU(cpu->has_mems && cpu->mems && cpu->mems[0], "cpuset.mems");
	CHECK_CPU(cpu->has_idle, "cpu.idle");
#undef CHECK_CPU
	return 0;
}

static int flux_runc_cgroup_v2_preflight_block_io(
	const char *leaf, const struct flux_oci_block_io_resources *block_io)
{
	bool needs_max = block_io->throttle_read_bps_num > 0 ||
			 block_io->throttle_write_bps_num > 0 ||
			 block_io->throttle_read_iops_num > 0 ||
			 block_io->throttle_write_iops_num > 0;
	bool needs_weight = block_io->has_weight;
	int i;
	int ret;

	if (block_io->has_leaf_weight)
		return -EOPNOTSUPP;
	for (i = 0; i < block_io->weight_device_num; i++) {
		ret = flux_runc_cgroup_check_block_device(
			block_io->weight_devices[i].major,
			block_io->weight_devices[i].minor);
		if (ret < 0)
			return ret;
		if (block_io->weight_devices[i].has_leaf_weight)
			return -EOPNOTSUPP;
		needs_weight |= block_io->weight_devices[i].has_weight;
	}
#define CHECK_THROTTLE_DEVICES(member, count)                                \
	do {                                                                   \
		for (i = 0; i < block_io->count; i++) {                          \
			ret = flux_runc_cgroup_check_block_device(                  \
				block_io->member[i].major, block_io->member[i].minor); \
			if (ret < 0)                                               \
				return ret;                                        \
		}                                                              \
	} while (0)
	CHECK_THROTTLE_DEVICES(throttle_read_bps, throttle_read_bps_num);
	CHECK_THROTTLE_DEVICES(throttle_write_bps, throttle_write_bps_num);
	CHECK_THROTTLE_DEVICES(throttle_read_iops, throttle_read_iops_num);
	CHECK_THROTTLE_DEVICES(throttle_write_iops, throttle_write_iops_num);
#undef CHECK_THROTTLE_DEVICES
	if (needs_weight &&
	    (ret = flux_runc_cgroup_check_writable(leaf, "io.weight")) < 0)
		return ret;
	if (needs_max &&
	    (ret = flux_runc_cgroup_check_writable(leaf, "io.max")) < 0)
		return ret;
	return 0;
}

static int flux_runc_cgroup_v2_preflight(
	const char *leaf, const struct flux_oci_resources *resources)
{
	int ret;

	if (resources->has_memory &&
	    (ret = flux_runc_cgroup_v2_preflight_memory(
		     leaf, &resources->memory)) < 0)
		return ret;
	if (resources->has_cpu &&
	    (ret = flux_runc_cgroup_v2_preflight_cpu(leaf,
						  &resources->cpu)) < 0)
		return ret;
	if (resources->has_pids && resources->pids.has_limit &&
	    (ret = flux_runc_cgroup_check_writable(leaf, "pids.max")) < 0)
		return ret;
	if (resources->has_block_io &&
	    (ret = flux_runc_cgroup_v2_preflight_block_io(
		     leaf, &resources->block_io)) < 0)
		return ret;
	return 0;
}

static int flux_runc_cgroup_write_i64(const char *dir, const char *name,
				      int64_t value, bool max_for_negative)
{
	char buf[64];

	if (value < 0 && max_for_negative) {
		if (snprintf(buf, sizeof(buf), "max\n") >= (int)sizeof(buf))
			return -EOVERFLOW;
	} else if (snprintf(buf, sizeof(buf), "%lld\n", (long long)value) >=
		   (int)sizeof(buf)) {
		return -EOVERFLOW;
	}
	return flux_runc_cgroup_write(dir, name, buf);
}

static int flux_runc_cgroup_write_u64(const char *dir, const char *name,
				      uint64_t value)
{
	char buf[64];

	if (snprintf(buf, sizeof(buf), "%llu\n",
		     (unsigned long long)value) >= (int)sizeof(buf))
		return -EOVERFLOW;
	return flux_runc_cgroup_write(dir, name, buf);
}

static uint64_t flux_runc_cpu_shares_to_weight(uint64_t shares)
{
	if (shares == 0)
		return 100;
	if (shares < 2)
		shares = 2;
	if (shares > 262144)
		shares = 262144;
	return 1 + ((shares - 2) * 9999) / 262142;
}

static uint64_t flux_runc_blkio_weight_v2(uint16_t weight)
{
	if (weight == 0)
		return 100;
	if (weight < 10)
		weight = 10;
	if (weight > 1000)
		weight = 1000;
	return 1 + ((uint64_t)(weight - 10) * 9999) / 990;
}

static int flux_runc_cgroup_check_block_device(int64_t major, int64_t minor)
{
	char path[PATH_MAX];
	struct stat st;

	if (major < 0 || minor < 0 ||
	    snprintf(path, sizeof(path), "/sys/dev/block/%lld:%lld",
		     (long long)major, (long long)minor) >= (int)sizeof(path))
		return -EINVAL;
	if (lstat(path, &st) < 0)
		return -errno;
	return 0;
}

static int flux_runc_cgroup_v2_apply_memory(
	const char *leaf, const struct flux_oci_memory_resources *memory)
{
	int ret;

	if (memory->has_limit) {
		ret = flux_runc_cgroup_write_i64(leaf, "memory.max",
						 memory->limit, true);
		if (ret < 0)
			return ret;
	}
	if (memory->has_reservation) {
		ret = flux_runc_cgroup_write_i64(
			leaf, "memory.low",
			memory->reservation < 0 ? 0 : memory->reservation, false);
		if (ret < 0)
			return ret;
	}
	if (memory->has_swap) {
		int64_t swap = memory->swap;

		if (swap >= 0 && memory->has_limit && memory->limit >= 0) {
			if (swap < memory->limit)
				return -EINVAL;
			swap -= memory->limit;
		} else if (swap >= 0 && !memory->has_limit) {
			return -EINVAL;
		}
		ret = flux_runc_cgroup_write_i64(leaf, "memory.swap.max", swap,
						 true);
		if (ret < 0)
			return ret;
	}
	if (memory->has_swappiness ||
	    (memory->has_disable_oom_killer && memory->disable_oom_killer) ||
	    memory->has_use_hierarchy)
		return -EOPNOTSUPP;
	return 0;
}

static int flux_runc_cgroup_v2_apply_cpu(
	const char *leaf, const struct flux_oci_cpu_resources *cpu)
{
	char value[128];
	uint64_t period;
	int ret;

	if (cpu->has_shares) {
		ret = flux_runc_cgroup_write_u64(
			leaf, "cpu.weight",
			flux_runc_cpu_shares_to_weight(cpu->shares));
		if (ret < 0)
			return ret;
	}
	if (cpu->has_quota || cpu->has_period) {
		period = cpu->has_period && cpu->period ? cpu->period : 100000;
		if (cpu->has_quota && cpu->quota >= 0) {
			if (snprintf(value, sizeof(value), "%lld %llu\n",
				     (long long)cpu->quota,
				     (unsigned long long)period) >= (int)sizeof(value))
				return -EOVERFLOW;
		} else if (snprintf(value, sizeof(value), "max %llu\n",
				    (unsigned long long)period) >= (int)sizeof(value)) {
			return -EOVERFLOW;
		}
		ret = flux_runc_cgroup_write(leaf, "cpu.max", value);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_burst) {
		ret = flux_runc_cgroup_write_u64(leaf, "cpu.max.burst",
						 cpu->burst);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_cpus && cpu->cpus && cpu->cpus[0]) {
		ret = flux_runc_cgroup_write(leaf, "cpuset.cpus", cpu->cpus);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_mems && cpu->mems && cpu->mems[0]) {
		ret = flux_runc_cgroup_write(leaf, "cpuset.mems", cpu->mems);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_idle) {
		ret = flux_runc_cgroup_write_i64(leaf, "cpu.idle", cpu->idle,
						 false);
		if (ret < 0)
			return ret;
	}
	if (cpu->has_realtime_runtime || cpu->has_realtime_period)
		return -EOPNOTSUPP;
	return 0;
}

static int flux_runc_cgroup_v2_apply_block_io(
	const char *leaf, const struct flux_oci_block_io_resources *block_io)
{
	char value[160];
	int i;
	int ret;

	if (block_io->has_weight) {
		if (snprintf(value, sizeof(value), "default %llu\n",
			     (unsigned long long)flux_runc_blkio_weight_v2(
				     block_io->weight)) >= (int)sizeof(value))
			return -EOVERFLOW;
		ret = flux_runc_cgroup_write(leaf, "io.weight", value);
		if (ret < 0)
			return ret;
	}
	if (block_io->has_leaf_weight)
		return -EOPNOTSUPP;
	for (i = 0; i < block_io->weight_device_num; i++) {
		const struct flux_oci_block_io_weight_device *dev =
			&block_io->weight_devices[i];
		if (dev->has_leaf_weight)
			return -EOPNOTSUPP;
		if (!dev->has_weight)
			continue;
		if (snprintf(value, sizeof(value), "%lld:%lld %llu\n",
			     (long long)dev->major, (long long)dev->minor,
			     (unsigned long long)flux_runc_blkio_weight_v2(
				     dev->weight)) >= (int)sizeof(value))
			return -EOVERFLOW;
		ret = flux_runc_cgroup_write(leaf, "io.weight", value);
		if (ret < 0)
			return ret;
	}
#define APPLY_THROTTLES(member, count, key)                                  \
	do {                                                                   \
		for (i = 0; i < block_io->count; i++) {                         \
			const struct flux_oci_block_io_throttle *limit =           \
				&block_io->member[i];                                \
			if (snprintf(value, sizeof(value),                       \
				     "%lld:%lld %s=%llu\n",                       \
				     (long long)limit->major,                         \
				     (long long)limit->minor, key,                    \
				     (unsigned long long)limit->rate) >=              \
			    (int)sizeof(value))                                      \
				return -EOVERFLOW;                                   \
			ret = flux_runc_cgroup_write(leaf, "io.max", value);      \
			if (ret < 0)                                               \
				return ret;                                           \
		}                                                              \
	} while (0)
	APPLY_THROTTLES(throttle_read_bps, throttle_read_bps_num, "rbps");
	APPLY_THROTTLES(throttle_write_bps, throttle_write_bps_num, "wbps");
	APPLY_THROTTLES(throttle_read_iops, throttle_read_iops_num, "riops");
	APPLY_THROTTLES(throttle_write_iops, throttle_write_iops_num, "wiops");
#undef APPLY_THROTTLES
	return 0;
}

static int flux_runc_cgroup_v2_apply(
	const char *leaf, const struct flux_oci_resources *resources)
{
	int ret;

	if (resources->has_memory) {
		ret = flux_runc_cgroup_v2_apply_memory(leaf, &resources->memory);
		if (ret < 0)
			return ret;
	}
	if (resources->has_cpu) {
		ret = flux_runc_cgroup_v2_apply_cpu(leaf, &resources->cpu);
		if (ret < 0)
			return ret;
	}
	if (resources->has_pids && resources->pids.has_limit) {
		ret = flux_runc_cgroup_write_i64(leaf, "pids.max",
						 resources->pids.limit, true);
		if (ret < 0)
			return ret;
	}
	if (resources->has_block_io) {
		ret = flux_runc_cgroup_v2_apply_block_io(leaf,
						    &resources->block_io);
		if (ret < 0)
			return ret;
	}
	/* cgroup v2 intentionally has no net_cls/net_prio controller. */
	return 0;
}

static int flux_runc_cgroup_v1_leaf(const char *root, const char *controller,
				    const struct flux_runc_state *state,
				    char *leaf, size_t size)
{
	struct flux_runc_cgroup_v1_controller descriptor = {
		.name = controller,
	};
	char prefix[PATH_MAX];

	if (!flux_runc_cgroup_v1_controller_active(root, &descriptor))
		return -EOPNOTSUPP;
	return flux_runc_cgroup_v1_paths(root, &descriptor, state, prefix,
					 sizeof(prefix), leaf, size);
}

static int flux_runc_cgroup_v1_apply_memory(
	const char *root, const struct flux_runc_state *state,
	const struct flux_oci_memory_resources *memory)
{
	char leaf[PATH_MAX];
	char value[64];
	int ret;

	ret = flux_runc_cgroup_v1_leaf(root, "memory", state, leaf,
					 sizeof(leaf));
	if (ret < 0)
		return ret;
	if (memory->has_limit &&
	    (ret = flux_runc_cgroup_write_i64(
		     leaf, "memory.limit_in_bytes", memory->limit, false)) < 0)
		return ret;
	if (memory->has_reservation &&
	    (ret = flux_runc_cgroup_write_i64(
		     leaf, "memory.soft_limit_in_bytes", memory->reservation,
		     false)) < 0)
		return ret;
	if (memory->has_swap &&
	    (ret = flux_runc_cgroup_write_i64(
		     leaf, "memory.memsw.limit_in_bytes", memory->swap,
		     false)) < 0)
		return ret;
	if (memory->has_swappiness &&
	    (ret = flux_runc_cgroup_write_u64(
		     leaf, "memory.swappiness", memory->swappiness)) < 0)
		return ret;
	if (memory->has_use_hierarchy &&
	    (ret = flux_runc_cgroup_write(
		     leaf, "memory.use_hierarchy",
		     memory->use_hierarchy ? "1\n" : "0\n")) < 0)
		return ret;
	if (memory->has_disable_oom_killer) {
		if (snprintf(value, sizeof(value), "%d\n",
			     memory->disable_oom_killer ? 1 : 0) >=
		    (int)sizeof(value))
			return -EOVERFLOW;
		ret = flux_runc_cgroup_write(leaf, "memory.oom_control", value);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int flux_runc_cgroup_v1_apply_cpu(
	const char *root, const struct flux_runc_state *state,
	const struct flux_oci_cpu_resources *cpu)
{
	char leaf[PATH_MAX];
	int ret;

	if (cpu->has_shares || cpu->has_quota || cpu->has_period ||
	    cpu->has_burst || cpu->has_realtime_runtime ||
	    cpu->has_realtime_period || cpu->has_idle) {
		ret = flux_runc_cgroup_v1_leaf(root, "cpu,cpuacct", state, leaf,
						 sizeof(leaf));
		if (ret < 0)
			return ret;
		if (cpu->has_shares &&
		    (ret = flux_runc_cgroup_write_u64(
			     leaf, "cpu.shares", cpu->shares)) < 0)
			return ret;
		if (cpu->has_quota &&
		    (ret = flux_runc_cgroup_write_i64(
			     leaf, "cpu.cfs_quota_us", cpu->quota, false)) < 0)
			return ret;
		if (cpu->has_period &&
		    (ret = flux_runc_cgroup_write_u64(
			     leaf, "cpu.cfs_period_us", cpu->period)) < 0)
			return ret;
		if (cpu->has_burst &&
		    (ret = flux_runc_cgroup_write_u64(
			     leaf, "cpu.cfs_burst_us", cpu->burst)) < 0)
			return ret;
		if (cpu->has_realtime_runtime &&
		    (ret = flux_runc_cgroup_write_i64(
			     leaf, "cpu.rt_runtime_us", cpu->realtime_runtime,
			     false)) < 0)
			return ret;
		if (cpu->has_realtime_period &&
		    (ret = flux_runc_cgroup_write_u64(
			     leaf, "cpu.rt_period_us", cpu->realtime_period)) < 0)
			return ret;
		if (cpu->has_idle)
			return -EOPNOTSUPP;
	}
	if (cpu->has_cpus || cpu->has_mems) {
		ret = flux_runc_cgroup_v1_leaf(root, "cpuset", state, leaf,
						 sizeof(leaf));
		if (ret < 0)
			return ret;
		if (cpu->has_cpus && cpu->cpus && cpu->cpus[0] &&
		    (ret = flux_runc_cgroup_write(leaf, "cpuset.cpus",
						   cpu->cpus)) < 0)
			return ret;
		if (cpu->has_mems && cpu->mems && cpu->mems[0] &&
		    (ret = flux_runc_cgroup_write(leaf, "cpuset.mems",
						   cpu->mems)) < 0)
			return ret;
	}
	return 0;
}

static int flux_runc_cgroup_v1_apply_block_io(
	const char *root, const struct flux_runc_state *state,
	const struct flux_oci_block_io_resources *block_io)
{
	char leaf[PATH_MAX];
	char value[128];
	int i;
	int ret;

	ret = flux_runc_cgroup_v1_leaf(root, "blkio", state, leaf,
					 sizeof(leaf));
	if (ret < 0)
		return ret;
	if (block_io->has_weight &&
	    (ret = flux_runc_cgroup_write_u64(
		     leaf, "blkio.weight", block_io->weight)) < 0)
		return ret;
	if (block_io->has_leaf_weight &&
	    (ret = flux_runc_cgroup_write_u64(
		     leaf, "blkio.leaf_weight", block_io->leaf_weight)) < 0)
		return ret;
	for (i = 0; i < block_io->weight_device_num; i++) {
		const struct flux_oci_block_io_weight_device *dev =
			&block_io->weight_devices[i];
		if (dev->has_weight) {
			if (snprintf(value, sizeof(value), "%lld:%lld %u\n",
				     (long long)dev->major,
				     (long long)dev->minor, dev->weight) >=
			    (int)sizeof(value))
				return -EOVERFLOW;
			ret = flux_runc_cgroup_write(leaf,
						     "blkio.weight_device", value);
			if (ret < 0)
				return ret;
		}
		if (dev->has_leaf_weight) {
			if (snprintf(value, sizeof(value), "%lld:%lld %u\n",
				     (long long)dev->major,
				     (long long)dev->minor,
				     dev->leaf_weight) >= (int)sizeof(value))
				return -EOVERFLOW;
			ret = flux_runc_cgroup_write(
				leaf, "blkio.leaf_weight_device", value);
			if (ret < 0)
				return ret;
		}
	}
#define APPLY_THROTTLES(member, count, name)                                 \
	do {                                                                   \
		for (i = 0; i < block_io->count; i++) {                         \
			const struct flux_oci_block_io_throttle *limit =           \
				&block_io->member[i];                                \
			if (snprintf(value, sizeof(value), "%lld:%lld %llu\n",  \
				     (long long)limit->major,                         \
				     (long long)limit->minor,                         \
				     (unsigned long long)limit->rate) >=              \
			    (int)sizeof(value))                                      \
				return -EOVERFLOW;                                   \
			ret = flux_runc_cgroup_write(leaf, name, value);          \
			if (ret < 0)                                               \
				return ret;                                           \
		}                                                              \
	} while (0)
	APPLY_THROTTLES(throttle_read_bps, throttle_read_bps_num,
		       "blkio.throttle.read_bps_device");
	APPLY_THROTTLES(throttle_write_bps, throttle_write_bps_num,
		       "blkio.throttle.write_bps_device");
	APPLY_THROTTLES(throttle_read_iops, throttle_read_iops_num,
		       "blkio.throttle.read_iops_device");
	APPLY_THROTTLES(throttle_write_iops, throttle_write_iops_num,
		       "blkio.throttle.write_iops_device");
#undef APPLY_THROTTLES
	return 0;
}

static int flux_runc_cgroup_v1_apply_network(
	const char *root, const struct flux_runc_state *state,
	const struct flux_oci_network_resources *network)
{
	char leaf[PATH_MAX];
	char value[PATH_MAX];
	int i;
	int ret;

	if (network->has_class_id) {
		ret = flux_runc_cgroup_v1_leaf(root, "net_cls,net_prio", state,
						 leaf, sizeof(leaf));
		if (ret < 0)
			return ret;
		if (snprintf(value, sizeof(value), "%u\n", network->class_id) >=
		    (int)sizeof(value))
			return -EOVERFLOW;
		ret = flux_runc_cgroup_write(leaf, "net_cls.classid", value);
		if (ret < 0)
			return ret;
	}
	if (network->priority_num > 0) {
		ret = flux_runc_cgroup_v1_leaf(root, "net_cls,net_prio", state,
						 leaf, sizeof(leaf));
		if (ret < 0)
			return ret;
		for (i = 0; i < network->priority_num; i++) {
			if (snprintf(value, sizeof(value), "%s %u\n",
				     network->priorities[i].name,
				     network->priorities[i].priority) >=
			    (int)sizeof(value))
				return -EOVERFLOW;
			ret = flux_runc_cgroup_write(leaf,
						     "net_prio.ifpriomap", value);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

static int flux_runc_cgroup_v1_apply(
	const char *root, const struct flux_runc_state *state,
	const struct flux_oci_resources *resources)
{
	char leaf[PATH_MAX];
	int ret;

	if (resources->has_memory) {
		ret = flux_runc_cgroup_v1_apply_memory(root, state,
						    &resources->memory);
		if (ret < 0)
			return ret;
	}
	if (resources->has_cpu) {
		ret = flux_runc_cgroup_v1_apply_cpu(root, state,
						 &resources->cpu);
		if (ret < 0)
			return ret;
	}
	if (resources->has_pids && resources->pids.has_limit) {
		ret = flux_runc_cgroup_v1_leaf(root, "pids", state, leaf,
						 sizeof(leaf));
		if (ret < 0)
			return ret;
		ret = flux_runc_cgroup_write_i64(leaf, "pids.max",
						 resources->pids.limit, true);
		if (ret < 0)
			return ret;
	}
	if (resources->has_block_io) {
		ret = flux_runc_cgroup_v1_apply_block_io(
			root, state, &resources->block_io);
		if (ret < 0)
			return ret;
	}
	if (resources->has_network) {
		ret = flux_runc_cgroup_v1_apply_network(
			root, state, &resources->network);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int flux_runc_cgroup_v1_check(
	const char *root, const struct flux_runc_state *state,
	const char *controller, const char *name)
{
	char leaf[PATH_MAX];
	int ret;

	ret = flux_runc_cgroup_v1_leaf(root, controller, state, leaf,
					 sizeof(leaf));
	if (ret < 0)
		return ret;
	return flux_runc_cgroup_check_writable(leaf, name);
}

static int flux_runc_cgroup_v1_preflight(
	const char *root, const struct flux_runc_state *state,
	const struct flux_oci_resources *resources)
{
	int i;
	int ret;

#define CHECK_V1(condition, controller, name)                                \
	do {                                                                   \
		if ((condition) &&                                               \
		    (ret = flux_runc_cgroup_v1_check(root, state, controller,     \
						 name)) < 0)                       \
			return ret;                                                \
	} while (0)
	if (resources->has_memory) {
		const struct flux_oci_memory_resources *memory =
			&resources->memory;

		CHECK_V1(memory->has_limit, "memory", "memory.limit_in_bytes");
		CHECK_V1(memory->has_reservation, "memory",
			 "memory.soft_limit_in_bytes");
		CHECK_V1(memory->has_swap, "memory",
			 "memory.memsw.limit_in_bytes");
		CHECK_V1(memory->has_swappiness, "memory", "memory.swappiness");
		CHECK_V1(memory->has_use_hierarchy, "memory",
			 "memory.use_hierarchy");
		CHECK_V1(memory->has_disable_oom_killer, "memory",
			 "memory.oom_control");
	}
	if (resources->has_cpu) {
		const struct flux_oci_cpu_resources *cpu = &resources->cpu;

		if (cpu->has_idle)
			return -EOPNOTSUPP;
		CHECK_V1(cpu->has_shares, "cpu,cpuacct", "cpu.shares");
		CHECK_V1(cpu->has_quota, "cpu,cpuacct", "cpu.cfs_quota_us");
		CHECK_V1(cpu->has_period, "cpu,cpuacct", "cpu.cfs_period_us");
		CHECK_V1(cpu->has_burst, "cpu,cpuacct", "cpu.cfs_burst_us");
		CHECK_V1(cpu->has_realtime_runtime, "cpu,cpuacct",
			 "cpu.rt_runtime_us");
		CHECK_V1(cpu->has_realtime_period, "cpu,cpuacct",
			 "cpu.rt_period_us");
		CHECK_V1(cpu->has_cpus && cpu->cpus && cpu->cpus[0], "cpuset",
			 "cpuset.cpus");
		CHECK_V1(cpu->has_mems && cpu->mems && cpu->mems[0], "cpuset",
			 "cpuset.mems");
	}
	CHECK_V1(resources->has_pids && resources->pids.has_limit, "pids",
		 "pids.max");
	if (resources->has_block_io) {
		const struct flux_oci_block_io_resources *block_io =
			&resources->block_io;

		CHECK_V1(block_io->has_weight, "blkio", "blkio.weight");
		CHECK_V1(block_io->has_leaf_weight, "blkio",
			 "blkio.leaf_weight");
		for (i = 0; i < block_io->weight_device_num; i++) {
			ret = flux_runc_cgroup_check_block_device(
				block_io->weight_devices[i].major,
				block_io->weight_devices[i].minor);
			if (ret < 0)
				return ret;
			CHECK_V1(block_io->weight_devices[i].has_weight, "blkio",
				 "blkio.weight_device");
			CHECK_V1(block_io->weight_devices[i].has_leaf_weight,
				 "blkio", "blkio.leaf_weight_device");
		}
#define CHECK_V1_THROTTLE_DEVICES(member, count)                             \
	do {                                                                   \
		for (i = 0; i < block_io->count; i++) {                          \
			ret = flux_runc_cgroup_check_block_device(                  \
				block_io->member[i].major, block_io->member[i].minor); \
			if (ret < 0)                                               \
				return ret;                                        \
		}                                                              \
	} while (0)
		CHECK_V1_THROTTLE_DEVICES(throttle_read_bps,
					  throttle_read_bps_num);
		CHECK_V1_THROTTLE_DEVICES(throttle_write_bps,
					  throttle_write_bps_num);
		CHECK_V1_THROTTLE_DEVICES(throttle_read_iops,
					  throttle_read_iops_num);
		CHECK_V1_THROTTLE_DEVICES(throttle_write_iops,
					  throttle_write_iops_num);
#undef CHECK_V1_THROTTLE_DEVICES
		CHECK_V1(block_io->throttle_read_bps_num > 0, "blkio",
			 "blkio.throttle.read_bps_device");
		CHECK_V1(block_io->throttle_write_bps_num > 0, "blkio",
			 "blkio.throttle.write_bps_device");
		CHECK_V1(block_io->throttle_read_iops_num > 0, "blkio",
			 "blkio.throttle.read_iops_device");
		CHECK_V1(block_io->throttle_write_iops_num > 0, "blkio",
			 "blkio.throttle.write_iops_device");
	}
	if (resources->has_network) {
		for (i = 0; i < resources->network.priority_num; i++)
			if (!resources->network.priorities[i].name ||
			    !if_nametoindex(
				    resources->network.priorities[i].name))
				return -ENODEV;
		CHECK_V1(resources->network.has_class_id, "net_cls,net_prio",
			 "net_cls.classid");
		CHECK_V1(resources->network.priority_num > 0,
			 "net_cls,net_prio", "net_prio.ifpriomap");
	}
#undef CHECK_V1
	return 0;
}

int flux_runc_cgroup_create(const struct flux_runc_state *state)
{
	char root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int ret;

	ret = flux_runc_cgroup_paths(state, prefix_path, sizeof(prefix_path),
				     leaf_path, sizeof(leaf_path));
	if (ret != 0)
		return ret > 0 ? 0 : ret;
	ret = flux_runc_cgroup_root(root, sizeof(root));
	if (ret < 0)
		return ret;
	if (!flux_runc_cgroup_v2(root))
		return flux_runc_cgroup_v1_create(root, state);
	ret = flux_runc_cgroup_check_parent(prefix_path);
	if (ret < 0)
		return ret;
	if (mkdir(leaf_path, 0755) < 0)
		return errno == EEXIST ? -EEXIST : -errno;
	return 0;
}

int flux_runc_cgroup_apply(const struct flux_runc_state *state,
			   const struct flux_oci_resources *resources)
{
	struct flux_runc_cgroup_txn txn = { 0 };
	char root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int ret;

	if (flux_runc_resources_empty(resources))
		return 0;
	ret = flux_runc_cgroup_paths(state, prefix_path, sizeof(prefix_path),
				     leaf_path, sizeof(leaf_path));
	if (ret != 0)
		return ret > 0 ? -EINVAL : ret;
	ret = flux_runc_cgroup_root(root, sizeof(root));
	if (ret < 0)
		return ret;
	flux_runc_cgroup_active_txn = &txn;
	if (flux_runc_cgroup_v2(root))
		ret = flux_runc_cgroup_v2_apply(leaf_path, resources);
	else
		ret = flux_runc_cgroup_v1_apply(root, state, resources);
	flux_runc_cgroup_active_txn = NULL;
	if (ret < 0)
		flux_runc_cgroup_txn_rollback(&txn);
	flux_runc_cgroup_txn_fini(&txn);
	return ret;
}

int flux_runc_cgroup_preflight(const struct flux_runc_state *state,
			       const struct flux_oci_resources *resources)
{
	char root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int ret;

	if (flux_runc_resources_empty(resources))
		return 0;
	ret = flux_runc_cgroup_paths(state, prefix_path, sizeof(prefix_path),
				     leaf_path, sizeof(leaf_path));
	if (ret != 0)
		return ret > 0 ? -EINVAL : ret;
	ret = flux_runc_cgroup_root(root, sizeof(root));
	if (ret < 0)
		return ret;
	if (flux_runc_cgroup_v2(root))
		return flux_runc_cgroup_v2_preflight(leaf_path, resources);
	return flux_runc_cgroup_v1_preflight(root, state, resources);
}

int flux_runc_cgroup_join(const struct flux_runc_state *state, pid_t pid)
{
	char root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int ret;

	ret = flux_runc_cgroup_paths(state, prefix_path, sizeof(prefix_path),
				     leaf_path, sizeof(leaf_path));
	if (ret != 0)
		return ret > 0 ? 0 : ret;
	if (pid <= 0)
		return -EINVAL;
	ret = flux_runc_cgroup_root(root, sizeof(root));
	if (ret < 0)
		return ret;
	if (!flux_runc_cgroup_v2(root))
		return flux_runc_cgroup_v1_join(root, state, pid);
	return flux_runc_cgroup_write_pid(leaf_path, "cgroup.procs", pid);
}

int flux_runc_cgroup_destroy(const struct flux_runc_state *state)
{
	char root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int ret;

	ret = flux_runc_cgroup_paths(state, prefix_path, sizeof(prefix_path),
				     leaf_path, sizeof(leaf_path));
	if (ret != 0)
		return ret > 0 ? 0 : ret;
	ret = flux_runc_cgroup_root(root, sizeof(root));
	if (ret < 0)
		return ret;
	if (!flux_runc_cgroup_v2(root))
		return flux_runc_cgroup_v1_destroy(root, state);
	if (rmdir(leaf_path) < 0 && errno != ENOENT)
		return -errno;
	return 0;
}

static int flux_runc_cgroup_read(const char *dir, const char *name, char *buf,
				 size_t size)
{
	char path[PATH_MAX];
	ssize_t nr;
	int fd;

	if (!buf || size < 2 ||
	    snprintf(path, sizeof(path), "%s/%s", dir, name) >=
		    (int)sizeof(path))
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		nr = read(fd, buf, size - 1);
	} while (nr < 0 && errno == EINTR);
	if (close(fd) < 0 && nr >= 0)
		return -errno;
	if (nr < 0)
		return -errno;
	buf[nr] = '\0';
	return 0;
}

static int flux_runc_cgroup_parse_u64(const char *value, uint64_t *out)
{
	char *end;
	unsigned long long parsed;

	if (!value || !out)
		return -EINVAL;
	while (*value == ' ' || *value == '\t' || *value == '\n')
		value++;
	if (!strncmp(value, "max", 3)) {
		*out = UINT64_MAX;
		return 0;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || end == value)
		return -EINVAL;
	*out = (uint64_t)parsed;
	return 0;
}

static uint64_t flux_runc_cgroup_stat_value(const char *buf,
					    const char *key)
{
	const char *line = buf;
	size_t key_len = strlen(key);

	while (line && *line) {
		const char *next = strchr(line, '\n');
		if (!strncmp(line, key, key_len) &&
		    (line[key_len] == ' ' || line[key_len] == '=')) {
			uint64_t value = 0;
			if (flux_runc_cgroup_parse_u64(line + key_len + 1,
						       &value) == 0)
				return value;
		}
		line = next ? next + 1 : NULL;
	}
	return 0;
}

static void flux_runc_cgroup_v2_io_totals(const char *buf, uint64_t *rbytes,
					  uint64_t *wbytes, uint64_t *rios,
					  uint64_t *wios)
{
	const char *line = buf;

	while (line && *line) {
		const char *next = strchr(line, '\n');
		const char *keys[] = { "rbytes=", "wbytes=", "rios=", "wios=" };
		uint64_t *values[] = { rbytes, wbytes, rios, wios };

		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			const char *pos = strstr(line, keys[i]);
			uint64_t value;

			if (!pos || (next && pos >= next))
				continue;
			if (flux_runc_cgroup_parse_u64(pos + strlen(keys[i]),
						       &value) == 0)
				*values[i] += value;
		}
		line = next ? next + 1 : NULL;
	}
}

static void flux_runc_cgroup_v1_io_totals(const char *buf, uint64_t *rbytes,
					  uint64_t *wbytes)
{
	const char *line = buf;

	while (line && *line) {
		char operation[32];
		unsigned long long value;
		int major;
		int minor;

		if (sscanf(line, "%d:%d %31s %llu", &major, &minor, operation,
			   &value) == 4) {
			if (!strcmp(operation, "Read"))
				*rbytes += (uint64_t)value;
			else if (!strcmp(operation, "Write"))
				*wbytes += (uint64_t)value;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
}

int flux_runc_cgroup_stats(const struct flux_runc_state *state, FILE *stream)
{
	uint64_t cpu_total = 0;
	uint64_t cpu_user = 0;
	uint64_t cpu_kernel = 0;
	uint64_t memory_current = 0;
	uint64_t memory_limit = UINT64_MAX;
	uint64_t pids_current = 0;
	uint64_t pids_limit = UINT64_MAX;
	uint64_t read_bytes = 0;
	uint64_t write_bytes = 0;
	uint64_t read_ops = 0;
	uint64_t write_ops = 0;
	char root[PATH_MAX];
	char prefix[PATH_MAX];
	char leaf[PATH_MAX];
	char buf[8192];
	bool v2;
	int ret;

	if (!state || !stream)
		return -EINVAL;
	ret = flux_runc_cgroup_paths(state, prefix, sizeof(prefix), leaf,
				     sizeof(leaf));
	if (ret != 0)
		return ret > 0 ? -ENOENT : ret;
	ret = flux_runc_cgroup_root(root, sizeof(root));
	if (ret < 0)
		return ret;
	v2 = flux_runc_cgroup_v2(root);
	if (v2) {
		if (flux_runc_cgroup_read(leaf, "cpu.stat", buf, sizeof(buf)) == 0) {
			cpu_total = flux_runc_cgroup_stat_value(buf, "usage_usec") *
				    1000;
			cpu_user = flux_runc_cgroup_stat_value(buf, "user_usec") *
				   1000;
			cpu_kernel = flux_runc_cgroup_stat_value(buf, "system_usec") *
				     1000;
		}
		if (flux_runc_cgroup_read(leaf, "memory.current", buf,
					  sizeof(buf)) == 0)
			(void)flux_runc_cgroup_parse_u64(buf, &memory_current);
		if (flux_runc_cgroup_read(leaf, "memory.max", buf,
					  sizeof(buf)) == 0)
			(void)flux_runc_cgroup_parse_u64(buf, &memory_limit);
		if (flux_runc_cgroup_read(leaf, "pids.current", buf,
					  sizeof(buf)) == 0)
			(void)flux_runc_cgroup_parse_u64(buf, &pids_current);
		if (flux_runc_cgroup_read(leaf, "pids.max", buf,
					  sizeof(buf)) == 0)
			(void)flux_runc_cgroup_parse_u64(buf, &pids_limit);
		if (flux_runc_cgroup_read(leaf, "io.stat", buf, sizeof(buf)) == 0)
			flux_runc_cgroup_v2_io_totals(
				buf, &read_bytes, &write_bytes, &read_ops, &write_ops);
	} else {
		if (flux_runc_cgroup_v1_leaf(root, "cpu,cpuacct", state, leaf,
						 sizeof(leaf)) == 0 &&
		    flux_runc_cgroup_read(leaf, "cpuacct.usage", buf,
					  sizeof(buf)) == 0)
			(void)flux_runc_cgroup_parse_u64(buf, &cpu_total);
		if (flux_runc_cgroup_v1_leaf(root, "memory", state, leaf,
						 sizeof(leaf)) == 0) {
			if (flux_runc_cgroup_read(leaf, "memory.usage_in_bytes", buf,
						  sizeof(buf)) == 0)
				(void)flux_runc_cgroup_parse_u64(buf,
							  &memory_current);
			if (flux_runc_cgroup_read(leaf, "memory.limit_in_bytes", buf,
						  sizeof(buf)) == 0)
				(void)flux_runc_cgroup_parse_u64(buf, &memory_limit);
		}
		if (flux_runc_cgroup_v1_leaf(root, "pids", state, leaf,
						 sizeof(leaf)) == 0) {
			if (flux_runc_cgroup_read(leaf, "pids.current", buf,
						  sizeof(buf)) == 0)
				(void)flux_runc_cgroup_parse_u64(buf, &pids_current);
			if (flux_runc_cgroup_read(leaf, "pids.max", buf,
						  sizeof(buf)) == 0)
				(void)flux_runc_cgroup_parse_u64(buf, &pids_limit);
		}
		if (flux_runc_cgroup_v1_leaf(root, "blkio", state, leaf,
						 sizeof(leaf)) == 0 &&
		    flux_runc_cgroup_read(leaf,
					  "blkio.throttle.io_service_bytes", buf,
					  sizeof(buf)) == 0)
			flux_runc_cgroup_v1_io_totals(buf, &read_bytes,
						       &write_bytes);
	}

	if (fprintf(stream,
		    "{\"cgroupVersion\":%d,"
		    "\"cpu\":{\"usage\":{\"total\":%llu,\"user\":%llu,"
		    "\"kernel\":%llu}},"
		    "\"memory\":{\"usage\":{\"usage\":%llu,\"limit\":%llu}},"
		    "\"pids\":{\"current\":%llu,\"limit\":%llu},"
		    "\"blkio\":{\"readBytes\":%llu,\"writeBytes\":%llu,"
		    "\"readOps\":%llu,\"writeOps\":%llu}}",
		    v2 ? 2 : 1, (unsigned long long)cpu_total,
		    (unsigned long long)cpu_user,
		    (unsigned long long)cpu_kernel,
		    (unsigned long long)memory_current,
		    (unsigned long long)memory_limit,
		    (unsigned long long)pids_current,
		    (unsigned long long)pids_limit,
		    (unsigned long long)read_bytes,
		    (unsigned long long)write_bytes,
		    (unsigned long long)read_ops,
		    (unsigned long long)write_ops) < 0)
		return -EIO;
	return 0;
}
