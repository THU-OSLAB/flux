#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <flux.h>
#include <flux/runtime.h>

#include "runc.h"
#ifndef FLUX_RUNC_RESOURCE_UNIT_TEST
#include "io/iok_client_internal.h"
#endif

#define FLUX_RUNC_MIN_MEMORY_BYTES (64ULL << 20)

static int flux_runc_resource_strdup(char **dst, const char *src)
{
	char *copy = src ? strdup(src) : NULL;

	if (src && !copy)
		return -ENOMEM;
	free(*dst);
	*dst = copy;
	return 0;
}

static void *flux_runc_resource_memdup(const void *src, size_t size)
{
	void *copy;

	if (!size)
		return NULL;
	copy = malloc(size);
	if (copy)
		memcpy(copy, src, size);
	return copy;
}

int flux_oci_resources_copy(struct flux_oci_resources *dst,
			    const struct flux_oci_resources *src)
{
	struct flux_oci_resources copy;
	int i;

	if (!dst || !src)
		return -EINVAL;
	memcpy(&copy, src, sizeof(copy));
	copy.cpu.cpus = NULL;
	copy.cpu.mems = NULL;
	copy.block_io.weight_devices = NULL;
	copy.block_io.throttle_read_bps = NULL;
	copy.block_io.throttle_write_bps = NULL;
	copy.block_io.throttle_read_iops = NULL;
	copy.block_io.throttle_write_iops = NULL;
	copy.network.priorities = NULL;

	if (src->cpu.cpus && flux_runc_resource_strdup(&copy.cpu.cpus,
						       src->cpu.cpus) < 0)
		goto nomem;
	if (src->cpu.mems && flux_runc_resource_strdup(&copy.cpu.mems,
						       src->cpu.mems) < 0)
		goto nomem;

#define COPY_ARRAY(member, count)                                            \
	do {                                                                   \
		if (src->member && src->count > 0) {                             \
			copy.member = flux_runc_resource_memdup(                    \
				src->member, sizeof(*src->member) *                 \
						     (size_t)src->count);             \
			if (!copy.member)                                           \
				goto nomem;                                         \
		}                                                              \
	} while (0)

	COPY_ARRAY(block_io.weight_devices, block_io.weight_device_num);
	COPY_ARRAY(block_io.throttle_read_bps, block_io.throttle_read_bps_num);
	COPY_ARRAY(block_io.throttle_write_bps,
		   block_io.throttle_write_bps_num);
	COPY_ARRAY(block_io.throttle_read_iops,
		   block_io.throttle_read_iops_num);
	COPY_ARRAY(block_io.throttle_write_iops,
		   block_io.throttle_write_iops_num);
#undef COPY_ARRAY

	if (src->network.priority_num > 0) {
		copy.network.priorities = calloc(
			(size_t)src->network.priority_num,
			sizeof(*copy.network.priorities));
		if (!copy.network.priorities)
			goto nomem;
		for (i = 0; i < src->network.priority_num; i++) {
			copy.network.priorities[i] = src->network.priorities[i];
			copy.network.priorities[i].name = NULL;
			if (flux_runc_resource_strdup(
				    &copy.network.priorities[i].name,
				    src->network.priorities[i].name) < 0)
				goto nomem;
		}
	}

	flux_oci_resources_fini(dst);
	*dst = copy;
	return 0;
nomem:
	flux_oci_resources_fini(&copy);
	return -ENOMEM;
}

static void flux_oci_memory_merge(struct flux_oci_memory_resources *dst,
				  const struct flux_oci_memory_resources *src)
{
#define MERGE_FIELD(name)                                                    \
	do {                                                                   \
		if (src->has_##name) {                                           \
			dst->name = src->name;                                     \
			dst->has_##name = true;                                    \
		}                                                              \
	} while (0)
	MERGE_FIELD(limit);
	MERGE_FIELD(reservation);
	MERGE_FIELD(swap);
	MERGE_FIELD(swappiness);
	MERGE_FIELD(disable_oom_killer);
	MERGE_FIELD(use_hierarchy);
	MERGE_FIELD(check_before_update);
#undef MERGE_FIELD
}

static int flux_oci_cpu_merge(struct flux_oci_cpu_resources *dst,
			      const struct flux_oci_cpu_resources *src)
{
#define MERGE_FIELD(name)                                                    \
	do {                                                                   \
		if (src->has_##name) {                                           \
			dst->name = src->name;                                     \
			dst->has_##name = true;                                    \
		}                                                              \
	} while (0)
	MERGE_FIELD(shares);
	MERGE_FIELD(quota);
	MERGE_FIELD(period);
	MERGE_FIELD(burst);
	MERGE_FIELD(realtime_runtime);
	MERGE_FIELD(realtime_period);
	MERGE_FIELD(idle);
#undef MERGE_FIELD
	if (src->has_cpus) {
		if (flux_runc_resource_strdup(&dst->cpus, src->cpus) < 0)
			return -ENOMEM;
		dst->has_cpus = true;
	}
	if (src->has_mems) {
		if (flux_runc_resource_strdup(&dst->mems, src->mems) < 0)
			return -ENOMEM;
		dst->has_mems = true;
	}
	return 0;
}

int flux_oci_resources_merge(struct flux_oci_resources *dst,
			     const struct flux_oci_resources *updates)
{
	struct flux_oci_resources merged = { 0 };
	struct flux_oci_resources replacement = { 0 };
	int ret;

	if (!dst || !updates)
		return -EINVAL;
	ret = flux_oci_resources_copy(&merged, dst);
	if (ret < 0)
		return ret;
	if (updates->has_memory) {
		flux_oci_memory_merge(&merged.memory, &updates->memory);
		merged.has_memory = true;
	}
	if (updates->has_cpu) {
		ret = flux_oci_cpu_merge(&merged.cpu, &updates->cpu);
		if (ret < 0)
			goto out;
		merged.has_cpu = true;
	}
	if (updates->has_pids) {
		merged.pids = updates->pids;
		merged.has_pids = true;
	}
	if (updates->has_block_io) {
		struct flux_oci_resources source = {
			.has_block_io = true,
			.block_io = updates->block_io,
		};

		ret = flux_oci_resources_copy(&replacement, &source);
		if (ret < 0)
			goto out;
		free(merged.block_io.weight_devices);
		free(merged.block_io.throttle_read_bps);
		free(merged.block_io.throttle_write_bps);
		free(merged.block_io.throttle_read_iops);
		free(merged.block_io.throttle_write_iops);
		merged.block_io = replacement.block_io;
		memset(&replacement.block_io, 0, sizeof(replacement.block_io));
		merged.has_block_io = true;
	}
	if (updates->has_network) {
		struct flux_oci_resources network_copy = { 0 };
		struct flux_oci_resources source = {
			.has_network = true,
			.network = updates->network,
		};

		ret = flux_oci_resources_copy(&network_copy, &source);
		if (ret < 0)
			goto out;
		for (int i = 0; i < merged.network.priority_num; i++)
			free(merged.network.priorities[i].name);
		free(merged.network.priorities);
		merged.network = network_copy.network;
		memset(&network_copy.network, 0, sizeof(network_copy.network));
		flux_oci_resources_fini(&network_copy);
		merged.has_network = true;
	}

	flux_oci_resources_fini(dst);
	*dst = merged;
	memset(&merged, 0, sizeof(merged));
	ret = 0;
out:
	flux_oci_resources_fini(&replacement);
	flux_oci_resources_fini(&merged);
	return ret;
}

static int flux_resource_json_string(FILE *stream, const char *value)
{
	const unsigned char *p;

	if (fputc('"', stream) == EOF)
		return -EIO;
	for (p = (const unsigned char *)(value ?: ""); *p; p++) {
		if (*p == '\\' || *p == '"') {
			if (fprintf(stream, "\\%c", *p) < 0)
				return -EIO;
		} else if (*p < 0x20) {
			if (fprintf(stream, "\\u%04x", *p) < 0)
				return -EIO;
		} else if (fputc(*p, stream) == EOF) {
			return -EIO;
		}
	}
	return fputc('"', stream) == EOF ? -EIO : 0;
}

static int flux_resource_json_sep(FILE *stream, bool *first)
{
	if (!*first && fputs(",", stream) == EOF)
		return -EIO;
	*first = false;
	return 0;
}

static int flux_resource_emit_memory(
	FILE *stream, const struct flux_oci_memory_resources *memory)
{
	bool first = true;

	if (fputc('{', stream) == EOF)
		return -EIO;
#define EMIT_I64(name, json_name)                                            \
	do {                                                                   \
		if (memory->has_##name) {                                        \
			if (flux_resource_json_sep(stream, &first) < 0 ||           \
			    fprintf(stream, "\"%s\":%lld", json_name,            \
				    (long long)memory->name) < 0)                  \
				return -EIO;                                         \
		}                                                              \
	} while (0)
	EMIT_I64(limit, "limit");
	EMIT_I64(reservation, "reservation");
	EMIT_I64(swap, "swap");
#undef EMIT_I64
	if (memory->has_swappiness &&
	    (flux_resource_json_sep(stream, &first) < 0 ||
	     fprintf(stream, "\"swappiness\":%llu",
		     (unsigned long long)memory->swappiness) < 0))
		return -EIO;
#define EMIT_BOOL(name, json_name)                                           \
	do {                                                                   \
		if (memory->has_##name &&                                        \
		    (flux_resource_json_sep(stream, &first) < 0 ||               \
		     fprintf(stream, "\"%s\":%s", json_name,                  \
			     memory->name ? "true" : "false") < 0))          \
			return -EIO;                                             \
	} while (0)
	EMIT_BOOL(disable_oom_killer, "disableOOMKiller");
	EMIT_BOOL(use_hierarchy, "useHierarchy");
	EMIT_BOOL(check_before_update, "checkBeforeUpdate");
#undef EMIT_BOOL
	return fputc('}', stream) == EOF ? -EIO : 0;
}

static int flux_resource_emit_cpu(FILE *stream,
				  const struct flux_oci_cpu_resources *cpu)
{
	bool first = true;

	if (fputc('{', stream) == EOF)
		return -EIO;
#define EMIT_U64(name, json_name)                                            \
	do {                                                                   \
		if (cpu->has_##name &&                                           \
		    (flux_resource_json_sep(stream, &first) < 0 ||               \
		     fprintf(stream, "\"%s\":%llu", json_name,                \
			     (unsigned long long)cpu->name) < 0))             \
			return -EIO;                                             \
	} while (0)
#define EMIT_I64(name, json_name)                                            \
	do {                                                                   \
		if (cpu->has_##name &&                                           \
		    (flux_resource_json_sep(stream, &first) < 0 ||               \
		     fprintf(stream, "\"%s\":%lld", json_name,                \
			     (long long)cpu->name) < 0))                      \
			return -EIO;                                             \
	} while (0)
	EMIT_U64(shares, "shares");
	EMIT_I64(quota, "quota");
	EMIT_U64(period, "period");
	EMIT_U64(burst, "burst");
	EMIT_I64(realtime_runtime, "realtimeRuntime");
	EMIT_U64(realtime_period, "realtimePeriod");
	EMIT_I64(idle, "idle");
#undef EMIT_U64
#undef EMIT_I64
	if (cpu->has_cpus) {
		if (flux_resource_json_sep(stream, &first) < 0 ||
		    fputs("\"cpus\":", stream) == EOF ||
		    flux_resource_json_string(stream, cpu->cpus) < 0)
			return -EIO;
	}
	if (cpu->has_mems) {
		if (flux_resource_json_sep(stream, &first) < 0 ||
		    fputs("\"mems\":", stream) == EOF ||
		    flux_resource_json_string(stream, cpu->mems) < 0)
			return -EIO;
	}
	return fputc('}', stream) == EOF ? -EIO : 0;
}

static int flux_resource_emit_throttles(
	FILE *stream, const struct flux_oci_block_io_throttle *items, int count)
{
	int i;

	if (fputc('[', stream) == EOF)
		return -EIO;
	for (i = 0; i < count; i++) {
		if (i && fputc(',', stream) == EOF)
			return -EIO;
		if (fprintf(stream,
			    "{\"major\":%lld,\"minor\":%lld,\"rate\":%llu}",
			    (long long)items[i].major, (long long)items[i].minor,
			    (unsigned long long)items[i].rate) < 0)
			return -EIO;
	}
	return fputc(']', stream) == EOF ? -EIO : 0;
}

static int flux_resource_emit_block_io(
	FILE *stream, const struct flux_oci_block_io_resources *block_io)
{
	bool first = true;
	int i;

	if (fputc('{', stream) == EOF)
		return -EIO;
	if (block_io->has_weight &&
	    (flux_resource_json_sep(stream, &first) < 0 ||
	     fprintf(stream, "\"weight\":%u", block_io->weight) < 0))
		return -EIO;
	if (block_io->has_leaf_weight &&
	    (flux_resource_json_sep(stream, &first) < 0 ||
	     fprintf(stream, "\"leafWeight\":%u", block_io->leaf_weight) < 0))
		return -EIO;
	if (block_io->weight_device_num > 0) {
		if (flux_resource_json_sep(stream, &first) < 0 ||
		    fputs("\"weightDevice\":[", stream) == EOF)
			return -EIO;
		for (i = 0; i < block_io->weight_device_num; i++) {
			const struct flux_oci_block_io_weight_device *dev =
				&block_io->weight_devices[i];
			if ((i && fputc(',', stream) == EOF) ||
			    fprintf(stream, "{\"major\":%lld,\"minor\":%lld",
				    (long long)dev->major,
				    (long long)dev->minor) < 0)
				return -EIO;
			if (dev->has_weight &&
			    fprintf(stream, ",\"weight\":%u", dev->weight) < 0)
				return -EIO;
			if (dev->has_leaf_weight &&
			    fprintf(stream, ",\"leafWeight\":%u",
				    dev->leaf_weight) < 0)
				return -EIO;
			if (fputc('}', stream) == EOF)
				return -EIO;
		}
		if (fputc(']', stream) == EOF)
			return -EIO;
	}
#define EMIT_THROTTLES(member, count, json_name)                             \
	do {                                                                   \
		if (block_io->count > 0) {                                      \
			if (flux_resource_json_sep(stream, &first) < 0 ||           \
			    fprintf(stream, "\"%s\":", json_name) < 0 ||          \
			    flux_resource_emit_throttles(                         \
				    stream, block_io->member, block_io->count) < 0)  \
				return -EIO;                                         \
		}                                                              \
	} while (0)
	EMIT_THROTTLES(throttle_read_bps, throttle_read_bps_num,
		       "throttleReadBpsDevice");
	EMIT_THROTTLES(throttle_write_bps, throttle_write_bps_num,
		       "throttleWriteBpsDevice");
	EMIT_THROTTLES(throttle_read_iops, throttle_read_iops_num,
		       "throttleReadIOPSDevice");
	EMIT_THROTTLES(throttle_write_iops, throttle_write_iops_num,
		       "throttleWriteIOPSDevice");
#undef EMIT_THROTTLES
	return fputc('}', stream) == EOF ? -EIO : 0;
}

static int flux_resource_emit_network(
	FILE *stream, const struct flux_oci_network_resources *network)
{
	bool first = true;
	int i;

	if (fputc('{', stream) == EOF)
		return -EIO;
	if (network->has_class_id &&
	    (flux_resource_json_sep(stream, &first) < 0 ||
	     fprintf(stream, "\"classID\":%u", network->class_id) < 0))
		return -EIO;
	if (network->priority_num > 0) {
		if (flux_resource_json_sep(stream, &first) < 0 ||
		    fputs("\"priorities\":[", stream) == EOF)
			return -EIO;
		for (i = 0; i < network->priority_num; i++) {
			if ((i && fputc(',', stream) == EOF) ||
			    fputs("{\"name\":", stream) == EOF ||
			    flux_resource_json_string(
				    stream, network->priorities[i].name) < 0 ||
			    fprintf(stream, ",\"priority\":%u}",
				    network->priorities[i].priority) < 0)
				return -EIO;
		}
		if (fputc(']', stream) == EOF)
			return -EIO;
	}
	return fputc('}', stream) == EOF ? -EIO : 0;
}

int flux_oci_resources_emit_json(const struct flux_oci_resources *resources,
				 FILE *stream)
{
	bool first = true;

	if (!resources || !stream || fputc('{', stream) == EOF)
		return -EINVAL;
#define EMIT_OBJECT(flag, name, fn, value)                                   \
	do {                                                                   \
		if (resources->flag) {                                           \
			if (flux_resource_json_sep(stream, &first) < 0 ||           \
			    fprintf(stream, "\"%s\":", name) < 0 ||               \
			    fn(stream, &resources->value) < 0)                      \
				return -EIO;                                         \
		}                                                              \
	} while (0)
	EMIT_OBJECT(has_memory, "memory", flux_resource_emit_memory, memory);
	EMIT_OBJECT(has_cpu, "cpu", flux_resource_emit_cpu, cpu);
	if (resources->has_pids &&
	    (flux_resource_json_sep(stream, &first) < 0 ||
	     fprintf(stream, "\"pids\":{\"limit\":%lld}",
		     (long long)resources->pids.limit) < 0))
		return -EIO;
	EMIT_OBJECT(has_block_io, "blockIO", flux_resource_emit_block_io,
		    block_io);
	EMIT_OBJECT(has_network, "network", flux_resource_emit_network, network);
#undef EMIT_OBJECT
	if (fputs("}\n", stream) == EOF)
		return -EIO;
	return 0;
}

static int flux_resource_write_atomic(const char *path, const char *buf,
				      size_t len)
{
	char tmp[PATH_MAX];
	int fd;
	int ret = 0;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >=
	    (int)sizeof(tmp))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	while (len > 0) {
		ssize_t nw = write(fd, buf, len);
		if (nw < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			break;
		}
		buf += nw;
		len -= (size_t)nw;
	}
	if (close(fd) < 0 && ret == 0)
		ret = -errno;
	if (ret == 0 && rename(tmp, path) < 0)
		ret = -errno;
	if (ret < 0)
		unlink(tmp);
	return ret;
}

int flux_runc_state_save_resources(const struct flux_runc_state *state,
				   const struct flux_oci_resources *resources)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *stream;
	int ret;

	if (!state || !state->resources_path || !resources)
		return -EINVAL;
	stream = open_memstream(&buf, &len);
	if (!stream)
		return -errno;
	ret = flux_oci_resources_emit_json(resources, stream);
	if (fclose(stream) < 0 && ret == 0)
		ret = -errno;
	if (ret == 0)
		ret = flux_resource_write_atomic(state->resources_path, buf, len);
	free(buf);
	return ret;
}

static int flux_resource_read_file(const char *path, char **out)
{
	struct stat st;
	char *buf;
	size_t off = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0 || st.st_size < 0 || st.st_size > 1024 * 1024) {
		int ret = errno ? -errno : -EFBIG;
		close(fd);
		return ret;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	while (off < (size_t)st.st_size) {
		ssize_t nr = read(fd, buf + off, (size_t)st.st_size - off);
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(fd);
			return -errno;
		}
		if (nr == 0)
			break;
		off += (size_t)nr;
	}
	close(fd);
	buf[off] = '\0';
	*out = buf;
	return 0;
}

int flux_runc_state_load_resources(const struct flux_runc_state *state,
				   struct flux_oci_resources *resources)
{
	char *json = NULL;
	int ret;

	if (!state || !state->resources_path || !resources)
		return -EINVAL;
	ret = flux_resource_read_file(state->resources_path, &json);
	if (ret == -ENOENT) {
		flux_oci_resources_fini(resources);
		return 0;
	}
	if (ret < 0)
		return ret;
	ret = flux_oci_resources_parse_json(json, resources);
	free(json);
	return ret;
}

static int flux_resource_cpu_set_count(const char *value, int *count)
{
	bool seen[CPU_SETSIZE] = { 0 };
	const char *p = value;
	int total = 0;

	if (!value || !value[0]) {
		*count = 0;
		return 0;
	}
	while (*p) {
		char *end;
		long first;
		long last;

		errno = 0;
		first = strtol(p, &end, 10);
		if (errno || end == p || first < 0 || first >= CPU_SETSIZE)
			return -EINVAL;
		last = first;
		p = end;
		if (*p == '-') {
			p++;
			errno = 0;
			last = strtol(p, &end, 10);
			if (errno || end == p || last < first || last >= CPU_SETSIZE)
				return -EINVAL;
			p = end;
		}
		for (long cpu = first; cpu <= last; cpu++) {
			if (!seen[cpu]) {
				seen[cpu] = true;
				total++;
			}
		}
		if (*p == ',')
			p++;
		else if (*p != '\0')
			return -EINVAL;
	}
	*count = total;
	return 0;
}

static int flux_resource_set_u64_string(char **dst, uint64_t value)
{
	char buf[32];

	if (snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value) >=
	    (int)sizeof(buf))
		return -EOVERFLOW;
	return flux_runc_resource_strdup(dst, buf);
}

static int flux_resource_set_i64_string(char **dst, int64_t value)
{
	char buf[32];

	if (snprintf(buf, sizeof(buf), "%lld", (long long)value) >=
	    (int)sizeof(buf))
		return -EOVERFLOW;
	return flux_runc_resource_strdup(dst, buf);
}

static bool flux_resource_block_io_configured(
	const struct flux_oci_block_io_resources *block_io)
{
	return block_io &&
	       (block_io->has_weight || block_io->has_leaf_weight ||
		block_io->weight_device_num > 0 ||
		block_io->throttle_read_bps_num > 0 ||
		block_io->throttle_write_bps_num > 0 ||
		block_io->throttle_read_iops_num > 0 ||
		block_io->throttle_write_iops_num > 0);
}

int flux_runc_resources_apply_run_config(
	const struct flux_runc_state *state,
	const struct flux_oci_resources *resources)
{
	struct flux_run_cfg cfg = { 0 };
	char *json = NULL;
	uint64_t cpu_count = 0;
	int cpuset_count = 0;
	int ret;

	if (!state || !state->run_config_path || !resources)
		return -EINVAL;
	ret = flux_resource_read_file(state->run_config_path, &json);
	if (ret < 0)
		return ret;
	ret = flux_run_cfg_load_json(&cfg, json);
	free(json);
	if (ret < 0)
		goto out;
	if (cfg.spdk_dev_num > 0 && resources->has_block_io &&
	    flux_resource_block_io_configured(&resources->block_io)) {
		flux_runc_set_error(
			"OCI blockIO cannot constrain the SPDK bypass path");
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (resources->has_memory && resources->memory.has_limit &&
	    resources->memory.limit > 0) {
		if ((uint64_t)resources->memory.limit < FLUX_RUNC_MIN_MEMORY_BYTES) {
			ret = -ERANGE;
			goto out;
		}
		ret = flux_resource_set_u64_string(
			&cfg.mem_size, (uint64_t)resources->memory.limit);
		if (ret < 0)
			goto out;
	}
	if (resources->has_cpu && resources->cpu.has_cpus &&
	    resources->cpu.cpus && resources->cpu.cpus[0]) {
		ret = flux_resource_cpu_set_count(resources->cpu.cpus,
						  &cpuset_count);
		if (ret < 0 || cpuset_count < 1 ||
		    cpuset_count > CONFIG_FLUX_MAX_CPUS) {
			ret = -ERANGE;
			goto out;
		}
		cpu_count = (uint64_t)cpuset_count;
		ret = flux_runc_resource_strdup(&cfg.cpu_set,
						 resources->cpu.cpus);
		if (ret < 0)
			goto out;
	} else if (resources->has_cpu && resources->cpu.has_quota &&
		   resources->cpu.quota > 0) {
		uint64_t period = resources->cpu.has_period &&
					  resources->cpu.period > 0 ?
					  resources->cpu.period : 100000;
		cpu_count = ((uint64_t)resources->cpu.quota + period - 1) /
			    period;
		if (cpu_count < 1)
			cpu_count = 1;
		if (cpu_count > CONFIG_FLUX_MAX_CPUS)
			cpu_count = CONFIG_FLUX_MAX_CPUS;
	}
	if (cpu_count > 0) {
		ret = flux_resource_set_u64_string(&cfg.nr_cpus, cpu_count);
		if (ret < 0)
			goto out;
	}
	if (resources->has_cpu) {
		const struct flux_oci_cpu_resources *cpu = &resources->cpu;

		if (cpu->has_shares) {
			ret = flux_resource_set_u64_string(&cfg.cpu_shares,
						   cpu->shares);
			if (ret < 0)
				goto out;
		}
		if (cpu->has_quota) {
			ret = flux_resource_set_i64_string(&cfg.cpu_quota,
						   cpu->quota);
			if (ret < 0)
				goto out;
		}
		if (cpu->has_period || cpu->has_quota) {
			ret = flux_resource_set_u64_string(&cfg.cpu_period,
						   cpu->has_period && cpu->period ?
							   cpu->period :
							   100000);
			if (ret < 0)
				goto out;
		}
	}
	if (resources->has_pids && resources->pids.has_limit) {
		ret = flux_resource_set_i64_string(&cfg.pid_limit,
						   resources->pids.limit);
		if (ret < 0)
			goto out;
	}
	if (resources->has_block_io) {
		if (resources->block_io.has_weight)
			ret = flux_resource_set_u64_string(
				&cfg.blkio_weight, resources->block_io.weight);
		else
			ret = flux_runc_resource_strdup(&cfg.blkio_weight, "0");
		if (ret < 0)
			goto out;
	}
	if (resources->has_network) {
		uint32_t priority = 0;

		if (resources->network.has_class_id)
			ret = flux_resource_set_u64_string(
				&cfg.network_class_id,
				resources->network.class_id);
		else
			ret = flux_runc_resource_strdup(&cfg.network_class_id, "0");
		if (ret < 0)
			goto out;
		for (int i = 0; i < resources->network.priority_num; i++)
			if (resources->network.priorities[i].priority > priority)
				priority =
					resources->network.priorities[i].priority;
		ret = flux_resource_set_u64_string(&cfg.network_priority,
						   priority);
		if (ret < 0)
			goto out;
	}
	ret = flux_run_cfg_save_json(&cfg, state->run_config_path);
out:
	flux_run_cfg_fini(&cfg);
	return ret;
}

#ifndef FLUX_RUNC_RESOURCE_UNIT_TEST
static void flux_runc_resource_limits_from_oci(
	const struct flux_oci_resources *resources,
	struct flux_resource_limits *limits)
{
	memset(limits, 0, sizeof(*limits));
	if (resources->has_memory) {
		const struct flux_oci_memory_resources *memory =
			&resources->memory;

		if (memory->has_limit) {
			limits->flags |= FLUX_RESOURCE_F_MEMORY_MAX;
			limits->memory_max = memory->limit;
		}
		if (memory->has_reservation) {
			limits->flags |= FLUX_RESOURCE_F_MEMORY_LOW;
			limits->memory_low = memory->reservation < 0 ? 0 :
							       memory->reservation;
		}
		if (memory->has_swap) {
			limits->flags |= FLUX_RESOURCE_F_MEMORY_SWAP_MAX;
			limits->memory_swap_max = memory->swap;
			if (limits->memory_swap_max >= 0 && memory->has_limit &&
			    memory->limit >= 0)
				limits->memory_swap_max -= memory->limit;
		}
	}
	if (resources->has_cpu) {
		const struct flux_oci_cpu_resources *cpu = &resources->cpu;

		if (cpu->has_shares) {
			uint64_t shares = cpu->shares;

			limits->flags |= FLUX_RESOURCE_F_CPU_WEIGHT;
			if (shares == 0)
				limits->cpu_weight = 100;
			else {
				if (shares < 2)
					shares = 2;
				if (shares > 262144)
					shares = 262144;
				limits->cpu_weight = 1 +
					((shares - 2) * 9999) / 262142;
			}
		}
		if (cpu->has_quota || cpu->has_period) {
			limits->flags |= FLUX_RESOURCE_F_CPU_MAX;
			limits->cpu_quota = cpu->has_quota ? cpu->quota : -1;
			limits->cpu_period = cpu->has_period && cpu->period ?
						     cpu->period : 100000;
		}
		if (cpu->has_burst) {
			limits->flags |= FLUX_RESOURCE_F_CPU_BURST;
			limits->cpu_burst = cpu->burst;
		}
		if (cpu->has_idle) {
			limits->flags |= FLUX_RESOURCE_F_CPU_IDLE;
			limits->cpu_idle = cpu->idle;
		}
	}
	if (resources->has_pids && resources->pids.has_limit) {
		limits->flags |= FLUX_RESOURCE_F_PIDS_MAX;
		limits->pids_max = resources->pids.limit;
	}
}

static int flux_runc_resource_wait(struct flux_resource_ctrl *resource,
				   uint64_t seq)
{
	struct timespec wait = {
		.tv_sec = 0,
		.tv_nsec = 10 * 1000 * 1000,
	};
	int waited_ms = 0;

	while (flux_resource_response_seq_load(resource) != seq) {
		if (waited_ms >= 5000)
			return -ETIMEDOUT;
		if (nanosleep(&wait, NULL) < 0 && errno != EINTR)
			return -errno;
		waited_ms += 10;
	}
	return resource->status;
}

static int flux_runc_resource_request(
	const struct flux_runc_state *state, uint32_t op,
	const struct flux_oci_resources *resources,
	struct flux_resource_stats *stats)
{
	struct flux_exec_ring *ring = NULL;
	struct flux_resource_ctrl *resource;
	uint64_t seq;
	int fd = -1;
	int ret;

	if (!state || !state->exec_ring_name)
		return -EINVAL;
	ret = flux_runc_exec_ring_open(state->exec_ring_name, false, false,
					 &ring, &fd);
	if (ret < 0)
		return ret;
	resource = &ring->resource;
	if (flux_resource_request_seq_load(resource) !=
	    flux_resource_response_seq_load(resource)) {
		ret = -EBUSY;
		goto out;
	}
	seq = flux_resource_request_seq_load(resource) + 1;
	if (!seq)
		seq = 1;
	memset(&resource->limits, 0, sizeof(resource->limits));
	memset(&resource->stats, 0, sizeof(resource->stats));
	if (resources)
		flux_runc_resource_limits_from_oci(resources,
						   &resource->limits);
	resource->status = -EINPROGRESS;
	resource->op = op;
	flux_resource_request_seq_store(resource, seq);
	ret = flux_runc_runner_resource_signal(state);
	if (ret < 0)
		goto out;
	ret = flux_runc_resource_wait(resource, seq);
	if (ret == 0 && stats)
		memcpy(stats, &resource->stats, sizeof(*stats));
out:
	flux_runc_exec_ring_close(ring, fd);
	return ret;
}

int flux_runc_resources_update_live(
	const struct flux_runc_state *state,
	const struct flux_oci_resources *resources)
{
	return flux_runc_resource_request(state, FLUX_RESOURCE_OP_UPDATE,
					  resources, NULL);
}

int flux_runc_resources_stats_live(
	const struct flux_runc_state *state,
	struct flux_resource_stats *stats)
{
	return flux_runc_resource_request(state, FLUX_RESOURCE_OP_STATS, NULL,
					  stats);
}

static int flux_runc_iok_socket_path(const struct flux_runc_state *state,
				     char **path_out)
{
	struct flux_run_cfg cfg = { 0 };
	char *json = NULL;
	char *path = NULL;
	int ret;

	ret = flux_resource_read_file(state->run_config_path, &json);
	if (ret < 0)
		return ret;
	ret = flux_run_cfg_load_json(&cfg, json);
	free(json);
	if (ret < 0)
		goto out;
	if (!cfg.iok_sock_path || !cfg.iok_sock_path[0] ||
	    !strcmp(cfg.iok_sock_path, FLUX_IOK_SOCK_PATH_BASE))
		path = flux_user_scoped_path_strdup(FLUX_IOK_SOCK_PATH_BASE);
	else
		path = strdup(cfg.iok_sock_path);
	if (!path)
		ret = -ENOMEM;
	else
		*path_out = path;
out:
	flux_run_cfg_fini(&cfg);
	return ret;
}

static int flux_runc_iok_connect(const char *path)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (!path || strlen(path) >= sizeof(addr.sun_path))
		return -ENAMETOOLONG;
	memcpy(addr.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int ret = -errno;
		close(fd);
		return ret;
	}
	return fd;
}

static int flux_runc_iok_resource_request(
	const struct flux_runc_state *state, uint32_t op,
	const struct flux_oci_resources *resources,
	struct flux_runc_iok_stats *stats)
{
	struct flux_iok_ctrl_resource_request request = {
		.hdr = {
			.magic = FLUX_IOK_CTRL_MAGIC,
			.version = FLUX_IOK_CTRL_VERSION,
			.op = op,
			.len = sizeof(request),
		},
		.peer_pid = state ? state->init_pid : 0,
	};
	struct flux_iok_ctrl_resource_reply reply;
	char *path = NULL;
	int fd = -1;
	int ret;

	if (!state || state->init_pid <= 0)
		return -ESRCH;
	if (resources) {
		if (resources->has_cpu) {
			if (resources->cpu.has_shares)
				request.cpu_shares = resources->cpu.shares;
			if (resources->cpu.has_quota)
				request.cpu_quota = resources->cpu.quota;
			if (resources->cpu.has_period || resources->cpu.has_quota)
				request.cpu_period =
					resources->cpu.has_period &&
						resources->cpu.period ?
						resources->cpu.period :
						100000;
		}
		if (resources->has_block_io &&
		    resources->block_io.has_weight)
			request.io_weight = resources->block_io.weight;
		if (resources->has_network) {
			if (resources->network.has_class_id)
				request.net_class_id =
					resources->network.class_id;
			for (int i = 0; i < resources->network.priority_num; i++)
				if (resources->network.priorities[i].priority >
				    request.net_priority)
					request.net_priority = resources->network
							       .priorities[i]
							       .priority;
		}
	}
	ret = flux_runc_iok_socket_path(state, &path);
	if (ret < 0)
		goto out;
	fd = flux_runc_iok_connect(path);
	if (fd < 0) {
		ret = fd;
		goto out;
	}
	ret = flux_iok_socket_send(fd, &request, sizeof(request), -1);
	if (ret < 0)
		goto out;
	ret = flux_iok_socket_recv(fd, &reply, sizeof(reply), NULL);
	if (ret < 0)
		goto out;
	if (reply.hdr.magic != FLUX_IOK_CTRL_MAGIC ||
	    reply.hdr.version != FLUX_IOK_CTRL_VERSION ||
	    reply.hdr.op != FLUX_IOK_CTRL_RESOURCE_REPLY ||
	    reply.hdr.len != sizeof(reply)) {
		ret = -EPROTO;
		goto out;
	}
	ret = reply.status;
	if (ret < 0 || !stats)
		goto out;
	memset(stats, 0, sizeof(*stats));
	stats->client_id = reply.client_id;
	stats->io_weight = reply.io_weight;
	stats->net_class_id = reply.net_class_id;
	stats->net_priority = reply.net_priority;
	stats->cpu_shares = reply.cpu_shares;
	stats->cpu_quota = reply.cpu_quota;
	stats->cpu_period = reply.cpu_period;
	stats->nr_cpus = reply.nr_cpus;
	for (unsigned int i = 0;
	     i < reply.nr_cpus && i < CONFIG_FLUX_MAX_CPUS; i++)
		stats->cpu_list[i] = reply.cpu_list[i];
	stats->rx_packets = reply.rx_packets;
	stats->rx_bytes = reply.rx_bytes;
	stats->tx_packets = reply.tx_packets;
	stats->tx_bytes = reply.tx_bytes;
out:
	if (fd >= 0)
		close(fd);
	free(path);
	return ret;
}

int flux_runc_resources_update_iokd(
	const struct flux_runc_state *state,
	const struct flux_oci_resources *resources)
{
	return flux_runc_iok_resource_request(
		state, FLUX_IOK_CTRL_RESOURCE_UPDATE, resources, NULL);
}

int flux_runc_resources_stats_iokd(
	const struct flux_runc_state *state,
	struct flux_runc_iok_stats *stats)
{
	return flux_runc_iok_resource_request(
		state, FLUX_IOK_CTRL_RESOURCE_STATS, NULL, stats);
}
#endif
