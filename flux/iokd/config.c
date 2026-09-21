#define FLUX_FMT "iokd: "

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kernel/asm/fnet.h>
#include <utils/log.h>
#include <utils/path.h>

#include <utils/jsmn.h>
#include "iokd.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct flux_iokd_config flux_iokd_cfg = {
	.cpu = -1,
	.mtu = FLUX_FNET_MTU,
};

struct flux_iokd_cfg_entry {
	const char *key;
	enum {
		IOKD_CFG_STRING,
		IOKD_CFG_INT,
		IOKD_CFG_BOOL,
		IOKD_CFG_CPU_RANGE,
	} type;
	void *dst;
};

static int flux_iokd_copy_string(char **dst, const char *src, size_t len)
{
	char *tmp;

	tmp = malloc(len + 1);
	if (!tmp)
		return -1;

	memcpy(tmp, src, len);
	tmp[len] = '\0';
	free(*dst);
	*dst = tmp;
	return 0;
}

static int flux_iokd_set_default_sock_path(void)
{
	char *path;

	path = flux_user_scoped_path_strdup(FLUX_IOK_SOCK_PATH_BASE);
	if (!path)
		return -1;

	free(flux_iokd_cfg.sock_path);
	flux_iokd_cfg.sock_path = path;
	return 0;
}

static const struct flux_iokd_cfg_entry *
flux_iokd_find_entry(const char *json, const jsmntok_t *tok)
{
	static const struct flux_iokd_cfg_entry entries[] = {
		{ "sock_path", IOKD_CFG_STRING, &flux_iokd_cfg.sock_path },
		{ "iok_sock_path", IOKD_CFG_STRING, &flux_iokd_cfg.sock_path },
		{ "nic_pci_addr", IOKD_CFG_STRING,
		  &flux_iokd_cfg.nic_pci_addr },
		{ "cpu_range", IOKD_CFG_CPU_RANGE, &flux_iokd_cfg.cpu_range },
		{ "mtu", IOKD_CFG_INT, &flux_iokd_cfg.mtu },
		{ "nic_mtu", IOKD_CFG_INT, &flux_iokd_cfg.mtu },
		{ "tx_copy", IOKD_CFG_BOOL, &flux_iokd_cfg.tx_copy },
		{ "tx_chksum_offload", IOKD_CFG_BOOL,
		  &flux_iokd_cfg.tx_chksum_offload },
		{ "no_network", IOKD_CFG_BOOL, &flux_iokd_cfg.no_network },
		{ "mlx5_external", IOKD_CFG_BOOL,
		  &flux_iokd_cfg.mlx5_external },
	};
	size_t i;
	size_t key_len;

	if (tok->type != JSMN_STRING)
		return NULL;

	key_len = (size_t)(tok->end - tok->start);
	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (strlen(entries[i].key) == key_len &&
		    strncmp(entries[i].key, json + tok->start, key_len) == 0)
			return &entries[i];
	}

	return NULL;
}

static int flux_iokd_parse_json(const char *json)
{
	jsmn_parser parser;
	jsmntok_t toks[128];
	int ret;
	int pos;

	jsmn_init(&parser);
	ret = jsmn_parse(&parser, json, strlen(json), toks, ARRAY_SIZE(toks));
	if (ret < 0)
		return -1;
	if (ret == 0 || toks[0].type != JSMN_OBJECT)
		return -1;

	for (pos = 1; pos < ret; pos++) {
		const struct flux_iokd_cfg_entry *entry;
		const jsmntok_t *val;

		entry = flux_iokd_find_entry(json, &toks[pos]);
		if (!entry)
			continue;
		if (pos + 1 >= ret)
			return -1;

		val = &toks[++pos];
		switch (entry->type) {
		case IOKD_CFG_STRING:
			if (val->type != JSMN_STRING)
				return -1;
			if (flux_iokd_copy_string(
				    entry->dst, json + val->start,
				    (size_t)(val->end - val->start)) < 0)
				return -1;
			break;
		case IOKD_CFG_INT: {
			char tmp[32];
			size_t len = (size_t)(val->end - val->start);

			if (len >= sizeof(tmp))
				return -1;
			memcpy(tmp, json + val->start, len);
			tmp[len] = '\0';
			*(int *)entry->dst = atoi(tmp);
			break;
		}
		case IOKD_CFG_BOOL:
			if (strncmp(json + val->start, "true",
				    (size_t)(val->end - val->start)) == 0 ||
			    strncmp(json + val->start, "1",
				    (size_t)(val->end - val->start)) == 0)
				*(bool *)entry->dst = true;
			else
				*(bool *)entry->dst = false;
			break;
		case IOKD_CFG_CPU_RANGE:
			if (val->type != JSMN_STRING)
				return -1;
			if (flux_iokd_copy_string(
				    entry->dst, json + val->start,
				    (size_t)(val->end - val->start)) < 0)
				return -1;
			break;
		}
	}

	return 0;
}

static int flux_iokd_load_config_file(const char *path)
{
	char *buf;
	int fd;
	off_t len;
	int ret = -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	len = lseek(fd, 0, SEEK_END);
	if (len < 0)
		goto out_close;
	if (lseek(fd, 0, SEEK_SET) < 0)
		goto out_close;

	buf = malloc((size_t)len + 1);
	if (!buf)
		goto out_close;

	if (read(fd, buf, (size_t)len) != len)
		goto out_free;
	buf[len] = '\0';

	ret = flux_iokd_parse_json(buf);

out_free:
	free(buf);
out_close:
	close(fd);
	return ret;
}

static void flux_iokd_print_usage(void)
{
	printf("Usage: flux-iokd -c <config.json> [--cpu-range SPEC] [--sock-path PATH] "
	       "[--nic-pci-addr BDF] [--tx-copy 0|1] "
	       "[--tx-chksum-offload 0|1] [--no-network 0|1] "
	       "[--mlx5-external 0|1]\n");
}

int flux_iokd_config_load(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "config", required_argument, 0, 'c' },
		{ "cpu-range", required_argument, 0, 1 },
		{ "sock-path", required_argument, 0, 2 },
		{ "nic-pci-addr", required_argument, 0, 3 },
		{ "tx-copy", required_argument, 0, 4 },
		{ "tx-chksum-offload", required_argument, 0, 5 },
		{ "no-network", required_argument, 0, 6 },
		{ "mlx5-external", required_argument, 0, 7 },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 },
	};
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "c:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (flux_iokd_copy_string(&flux_iokd_cfg.config_path,
						  optarg, strlen(optarg)) < 0)
				return -1;
			break;
		case 'h':
			flux_iokd_print_usage();
			return -1;
		default:
			break;
		}
	}

	if (flux_iokd_cfg.config_path &&
	    flux_iokd_load_config_file(flux_iokd_cfg.config_path) < 0)
		return -1;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "c:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			break;
		case 'h':
			break;
		case 1:
			if (flux_iokd_copy_string(&flux_iokd_cfg.cpu_range,
						  optarg, strlen(optarg)) < 0)
				return -1;
			break;
		case 2:
			if (flux_iokd_copy_string(&flux_iokd_cfg.sock_path,
						  optarg, strlen(optarg)) < 0)
				return -1;
			break;
		case 3:
			if (flux_iokd_copy_string(&flux_iokd_cfg.nic_pci_addr,
						  optarg, strlen(optarg)) < 0)
				return -1;
			break;
		case 4:
			flux_iokd_cfg.tx_copy = atoi(optarg) != 0;
			break;
		case 5:
			flux_iokd_cfg.tx_chksum_offload = atoi(optarg) != 0;
			break;
		case 6:
			flux_iokd_cfg.no_network = atoi(optarg) != 0;
			break;
		case 7:
			flux_iokd_cfg.mlx5_external = atoi(optarg) != 0;
			break;
		default:
			flux_iokd_print_usage();
			return -1;
		}
	}

	if (flux_iokd_cfg.no_network && flux_iokd_cfg.nic_pci_addr &&
	    flux_iokd_cfg.nic_pci_addr[0]) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "no_network cannot be combined with nic_pci_addr\n");
		return -1;
	}
	if (flux_iokd_cfg.mlx5_external &&
	    (!flux_iokd_cfg.nic_pci_addr || !flux_iokd_cfg.nic_pci_addr[0])) {
		FLUX_LOG(FLUX_LOG_ERR, "mlx5_external requires nic_pci_addr\n");
		return -1;
	}

	if (!flux_iokd_cfg.sock_path ||
	    strcmp(flux_iokd_cfg.sock_path, FLUX_IOK_SOCK_PATH_BASE) == 0) {
		if (flux_iokd_set_default_sock_path() < 0)
			return -1;
	}

	if (!flux_iokd_cfg.cpu_range || !flux_iokd_cfg.cpu_range[0]) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"missing flux_iokd cpu_range; specify --cpu-range or set cpu_range in config\n");
		return -1;
	}

	return 0;
}

void flux_iokd_config_cleanup(void)
{
	free(flux_iokd_cfg.config_path);
	flux_iokd_cfg.config_path = NULL;
	free(flux_iokd_cfg.sock_path);
	flux_iokd_cfg.sock_path = NULL;
	free(flux_iokd_cfg.nic_pci_addr);
	flux_iokd_cfg.nic_pci_addr = NULL;
	free(flux_iokd_cfg.cpu_range);
	flux_iokd_cfg.cpu_range = NULL;
}
