#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runc.h"

#define FLUX_RUNC_UPDATE_MAX_JSON (1024U * 1024U)

static int flux_runc_update_read_fd(int fd, char **out)
{
	char *buf = NULL;
	size_t capacity = 4096;
	size_t len = 0;

	buf = malloc(capacity);
	if (!buf)
		return -ENOMEM;
	for (;;) {
		ssize_t nr;

		if (len == capacity) {
			char *grown;
			if (capacity >= FLUX_RUNC_UPDATE_MAX_JSON) {
				free(buf);
				return -EFBIG;
			}
			capacity *= 2;
			if (capacity > FLUX_RUNC_UPDATE_MAX_JSON)
				capacity = FLUX_RUNC_UPDATE_MAX_JSON;
			grown = realloc(buf, capacity + 1);
			if (!grown) {
				free(buf);
				return -ENOMEM;
			}
			buf = grown;
		}
		nr = read(fd, buf + len, capacity - len);
		if (nr < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -errno;
		}
		if (nr == 0)
			break;
		len += (size_t)nr;
	}
	buf[len] = '\0';
	*out = buf;
	return 0;
}

static int flux_runc_update_read(const char *path, char **out)
{
	int fd;
	int ret;

	if (!strcmp(path, "-"))
		return flux_runc_update_read_fd(STDIN_FILENO, out);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ret = flux_runc_update_read_fd(fd, out);
	close(fd);
	return ret;
}

int flux_runc_cmd_update(int argc, char **argv)
{
	static struct option options[] = {
		{ "resources", required_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct flux_oci_resources current = { 0 };
	struct flux_oci_resources previous = { 0 };
	struct flux_oci_resources updates = { 0 };
	struct flux_runc_state state;
	const char *resources_path = NULL;
	char *json = NULL;
	bool cgroup_touched = false;
	bool iokd_touched = false;
	bool live_touched = false;
	bool resources_saved = false;
	bool run_config_touched = false;
	int opt;
	int ret;

	optind = 2;
	while ((opt = getopt_long(argc, argv, "+r:h", options, NULL)) != -1) {
		switch (opt) {
		case 'r':
			resources_path = optarg;
			break;
		case 'h':
		default:
			return EXIT_FAILURE;
		}
	}
	if (!resources_path || argc - optind != 1)
		return EXIT_FAILURE;

	ret = flux_runc_update_read(resources_path, &json);
	if (ret < 0)
		goto fail;
	ret = flux_oci_resources_parse_json(json, &updates);
	if (ret < 0) {
		ret = -EINVAL;
		goto fail;
	}
	ret = flux_runc_state_load(&state, argv[optind]);
	if (ret < 0)
		goto fail;
	ret = flux_runc_state_reconcile(&state);
	if (ret < 0)
		goto fail_state;
	if (state.status != FLUX_RUNC_CREATED &&
	    state.status != FLUX_RUNC_RUNNING) {
		ret = -EBUSY;
		goto fail_state;
	}
	if (state.status == FLUX_RUNC_RUNNING && updates.has_cpu &&
	    (updates.cpu.has_cpus || updates.cpu.has_mems)) {
		ret = -EOPNOTSUPP;
		goto fail_state;
	}
	ret = flux_runc_state_load_resources(&state, &current);
	if (ret < 0)
		goto fail_state;
	ret = flux_oci_resources_copy(&previous, &current);
	if (ret < 0)
		goto fail_state;
	ret = flux_oci_resources_merge(&current, &updates);
	if (ret < 0)
		goto fail_state;
	ret = flux_runc_cgroup_preflight(&state, &current);
	if (ret < 0)
		goto fail_state;
	if (state.status == FLUX_RUNC_RUNNING) {
		iokd_touched = true;
		ret = flux_runc_resources_update_iokd(&state, &current);
		if (ret < 0)
			goto rollback;
		live_touched = true;
		ret = flux_runc_resources_update_live(&state, &current);
		if (ret < 0)
			goto rollback;
	} else {
		run_config_touched = true;
		ret = flux_runc_resources_apply_run_config(&state, &current);
		if (ret < 0)
			goto rollback;
	}
	ret = flux_runc_state_save_resources(&state, &current);
	if (ret < 0)
		goto rollback;
	resources_saved = true;
	cgroup_touched = true;
	ret = flux_runc_cgroup_apply(&state, &current);
	if (ret < 0)
		goto rollback;

	flux_runc_state_fini(&state);
	flux_oci_resources_fini(&previous);
	flux_oci_resources_fini(&current);
	flux_oci_resources_fini(&updates);
	free(json);
	return EXIT_SUCCESS;

rollback:
	/* Preserve the first failure; compensation is deliberately best-effort. */
	if (cgroup_touched)
		(void)flux_runc_cgroup_apply(&state, &previous);
	if (resources_saved)
		(void)flux_runc_state_save_resources(&state, &previous);
	if (live_touched)
		(void)flux_runc_resources_update_live(&state, &previous);
	if (iokd_touched)
		(void)flux_runc_resources_update_iokd(&state, &previous);
	if (run_config_touched)
		(void)flux_runc_resources_apply_run_config(&state, &previous);
fail_state:
	flux_runc_state_fini(&state);
fail:
	flux_runc_log_errno("failed to update container resources", ret);
	flux_oci_resources_fini(&previous);
	flux_oci_resources_fini(&current);
	flux_oci_resources_fini(&updates);
	free(json);
	return EXIT_FAILURE;
}
