#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runc.h"

int flux_runc_stats_emit(const char *container_id, FILE *stream)
{
	struct flux_oci_resources resources = { 0 };
	struct flux_resource_stats internal = { 0 };
	struct flux_runc_iok_stats iok = { 0 };
	struct flux_runc_state state;
	bool has_internal = false;
	bool has_iok = false;
	int ret;

	if (!container_id || !stream)
		return -EINVAL;
	ret = flux_runc_state_load(&state, container_id);
	if (ret < 0)
		return ret;
	ret = flux_runc_state_reconcile(&state);
	if (ret < 0)
		goto out;
	ret = flux_runc_state_load_resources(&state, &resources);
	if (ret < 0)
		goto out;
	if (state.status == FLUX_RUNC_RUNNING &&
	    flux_runc_resources_stats_live(&state, &internal) == 0)
		has_internal = true;
	if (state.status == FLUX_RUNC_RUNNING &&
	    flux_runc_resources_stats_iokd(&state, &iok) == 0)
		has_iok = true;

	if (fprintf(stream, "{\"type\":\"stats\",\"id\":\"%s\",\"data\":",
		    state.id) < 0) {
		ret = -EIO;
		goto out;
	}
	ret = flux_runc_cgroup_stats(&state, stream);
	if (ret < 0)
		goto out;
	if (fprintf(stream,
		    ",\"fluxInternal\":{\"available\":%s,"
		    "\"memory\":{\"current\":%llu,\"limit\":%llu,"
		    "\"oom\":%llu,\"oomKill\":%llu},"
		    "\"cpu\":{\"usageUsec\":%llu,\"userUsec\":%llu,"
		    "\"systemUsec\":%llu,\"throttledUsec\":%llu},"
		    "\"pids\":{\"current\":%llu,\"limit\":%llu,"
		    "\"maxEvents\":%llu},"
		    "\"io\":{\"readBytes\":%llu,\"writeBytes\":%llu,"
		    "\"readOps\":%llu,\"writeOps\":%llu}}",
		    has_internal ? "true" : "false",
		    (unsigned long long)internal.memory_current,
		    (unsigned long long)internal.memory_max,
		    (unsigned long long)internal.memory_events_oom,
		    (unsigned long long)internal.memory_events_oom_kill,
		    (unsigned long long)internal.cpu_usage_usec,
		    (unsigned long long)internal.cpu_user_usec,
		    (unsigned long long)internal.cpu_system_usec,
		    (unsigned long long)internal.cpu_throttled_usec,
		    (unsigned long long)internal.pids_current,
		    (unsigned long long)internal.pids_max,
		    (unsigned long long)internal.pids_events_max,
		    (unsigned long long)internal.io_read_bytes,
		    (unsigned long long)internal.io_write_bytes,
		    (unsigned long long)internal.io_read_ops,
		    (unsigned long long)internal.io_write_ops) < 0) {
		ret = -EIO;
		goto out;
	}
	if (fprintf(stream,
		    ",\"iokd\":{\"available\":%s,\"clientID\":%d,"
		    "\"cpuCount\":%u,\"ioWeight\":%u,\"networkClassID\":%u,"
		    "\"networkPriority\":%u,\"cpuShares\":%u,"
		    "\"cpuQuota\":%lld,\"cpuPeriod\":%llu,"
		    "\"rxPackets\":%llu,\"rxBytes\":%llu,"
		    "\"txPackets\":%llu,\"txBytes\":%llu}",
		    has_iok ? "true" : "false", iok.client_id, iok.nr_cpus,
		    iok.io_weight, iok.net_class_id, iok.net_priority,
		    iok.cpu_shares, (long long)iok.cpu_quota,
		    (unsigned long long)iok.cpu_period,
		    (unsigned long long)iok.rx_packets,
		    (unsigned long long)iok.rx_bytes,
		    (unsigned long long)iok.tx_packets,
		    (unsigned long long)iok.tx_bytes) < 0) {
		ret = -EIO;
		goto out;
	}
	if (fputs(",\"fluxResources\":", stream) == EOF) {
		ret = -EIO;
		goto out;
	}
	ret = flux_oci_resources_emit_json(&resources, stream);
	if (ret < 0)
		goto out;
	if (fputs("}\n", stream) == EOF)
		ret = -EIO;
out:
	flux_oci_resources_fini(&resources);
	flux_runc_state_fini(&state);
	return ret;
}

int flux_runc_cmd_stats(int argc, char **argv)
{
	int ret;

	if (argc != 3)
		return EXIT_FAILURE;
	ret = flux_runc_stats_emit(argv[2], stdout);
	if (ret < 0) {
		flux_runc_log_errno("failed to collect container stats", ret);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
