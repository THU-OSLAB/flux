#define FLUX_FMT "flux_iokd: "

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flux.h>
#include <kernel/asm/fnet.h>

#include "iok_client.h"

#define FLUX_FNET_CTRL_DEV_PATH "/dev/fnet-control"
#define FLUX_FNET_CTRL_SYSFS_DEV_PATH "/sys/class/misc/fnet-control/dev"

static inline bool flux_fnet_is_configured(void)
{
	return flux_env.fnet_enabled;
}

static int netmask_to_prefix(const char *mask_str)
{
	struct in_addr addr;
	uint32_t mask;
	int prefix_len = 0;

	if (inet_pton(AF_INET, mask_str, &addr) != 1)
		return -1;

	mask = ntohl(addr.s_addr);
	while (mask & 0x80000000) {
		prefix_len++;
		mask <<= 1;
	}

	if (mask != 0)
		return -1;

	return prefix_len;
}

static int flux_fnet_config_dev(int ifindex, const struct flux_fnet_netdev *dev)
{
	int ret;
	int nmlen;
	unsigned int addr;
#ifndef CONFIG_FLUX_FAST_NET
	unsigned int gwaddr;
#endif

	ret = flux_if_up(ifindex);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to bring up fnet netdev: %d\n", ret);
		goto out;
	}

	if (dev->mtu) {
		ret = flux_if_set_mtu(ifindex, (int)dev->mtu);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to set mtu on fnet netdev: %d\n", ret);
			goto out;
		}
	}

	if (run_cfg->nic_ip_addr) {
		if (inet_pton(FLUX_AF_INET, run_cfg->nic_ip_addr,
			      (struct flux_in_addr *)&addr) != 1) {
			FLUX_LOG(FLUX_LOG_ERR, "invalid nic_ip_addr: %s\n",
				 run_cfg->nic_ip_addr);
			ret = -FLUX_EINVAL;
			goto out;
		}

		nmlen = run_cfg->nic_ip_mask ? netmask_to_prefix(run_cfg->nic_ip_mask) :
					      24;
		if (addr != FLUX_INADDR_NONE && nmlen > 0 && nmlen < 32) {
			ret = flux_if_set_ipv4(ifindex, addr, nmlen);
			if (ret < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "failed to set ip addr on fnet netdev: %d\n",
					 ret);
				goto out;
			}
		}

		/*
		 * CONFIG_FLUX_FAST_NET routes packets directly with the gateway
		 * stored in the fnet device. Its replacement socket layer does not
		 * implement the netlink route operation below, and the failed request
		 * would make an otherwise healthy device registration look fatal.
		 */
#ifndef CONFIG_FLUX_FAST_NET
		if (run_cfg->nic_ip_gw) {
			if (inet_pton(FLUX_AF_INET, run_cfg->nic_ip_gw,
				      (struct flux_in_addr *)&gwaddr) != 1) {
				FLUX_LOG(FLUX_LOG_ERR, "invalid nic_ip_gw: %s\n",
					 run_cfg->nic_ip_gw);
				ret = -FLUX_EINVAL;
				goto out;
			}

			if (gwaddr != FLUX_INADDR_NONE) {
				ret = flux_if_set_ipv4_gateway(ifindex, addr,
							       nmlen, gwaddr);
				if (ret < 0) {
					FLUX_LOG(FLUX_LOG_ERR,
						 "failed to set gateway on fnet netdev: %d\n",
						 ret);
					goto out;
				}
			}
		}
#endif
	}

	return 0;
out:
	return ret;
}

static void flux_fnet_free_dev(struct flux_fnet_netdev *dev)
{
	free(dev->rxqs);
	free(dev->txpktqs);
	free(dev->txcmdqs);
	dev->rxqs = NULL;
	dev->txpktqs = NULL;
	dev->txcmdqs = NULL;
}

static int flux_fnet_mknod_ctrl_dev(void)
{
	int ret;
	int fd;
	unsigned int major;
	unsigned int minor;
	char devno[32];

	fd = flux_sys_open(FLUX_FNET_CTRL_SYSFS_DEV_PATH, FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s: %s\n",
			 FLUX_FNET_CTRL_SYSFS_DEV_PATH, flux_strerror(fd));
		return fd;
	}

	ret = flux_sys_read(fd, devno, sizeof(devno) - 1);
	flux_sys_close(fd);
	if (ret <= 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to read %s: %s\n",
			 FLUX_FNET_CTRL_SYSFS_DEV_PATH,
			 ret < 0 ? flux_strerror(ret) : "empty file");
		return ret < 0 ? ret : -FLUX_EINVAL;
	}
	devno[ret] = '\0';

	if (sscanf(devno, "%u:%u", &major, &minor) != 2) {
		FLUX_LOG(FLUX_LOG_ERR, "invalid device number format: %s\n",
			 devno);
		return -FLUX_EINVAL;
	}

	ret = flux_sys_mknod(FLUX_FNET_CTRL_DEV_PATH, FLUX_S_IFCHR | 0600,
			     FLUX_MKDEV(major, minor));
	if (ret == -FLUX_EEXIST) {
		ret = flux_sys_unlink(FLUX_FNET_CTRL_DEV_PATH);
		if (ret < 0 && ret != -FLUX_ENOENT) {
			FLUX_LOG(FLUX_LOG_ERR, "failed to unlink %s: %s\n",
				 FLUX_FNET_CTRL_DEV_PATH, flux_strerror(ret));
			return ret;
		}

		ret = flux_sys_mknod(FLUX_FNET_CTRL_DEV_PATH,
				     FLUX_S_IFCHR | 0600,
				     FLUX_MKDEV(major, minor));
	}

	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mknod %s (%u:%u): %s\n",
			 FLUX_FNET_CTRL_DEV_PATH, major, minor,
			 flux_strerror(ret));
		return ret;
	}

	return 0;
}

int flux_fnet_register_dev(void)
{
	struct flux_fnet_netdev dev = {
		.port_id = FLUX_FNET_PORT,
	};
	int fd;
	int ret;

	if (!flux_fnet_is_configured())
		return 0;

	ret = flux_iok_client_fill_netdev(&dev);
	if (ret < 0)
		return ret;

	fd = flux_sys_open(FLUX_FNET_CTRL_DEV_PATH, FLUX_O_RDWR, 0);
	if (fd < 0) {
		ret = flux_fnet_mknod_ctrl_dev();
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_ERR, "failed to create %s\n",
				 FLUX_FNET_CTRL_DEV_PATH);
			goto out_free;
		}

		fd = flux_sys_open(FLUX_FNET_CTRL_DEV_PATH, FLUX_O_RDWR, 0);
		if (fd < 0) {
			ret = -FLUX_ENODEV;
			FLUX_LOG(FLUX_LOG_ERR, "failed to open %s after mknod\n",
				 FLUX_FNET_CTRL_DEV_PATH);
			goto out_free;
		}
	}

	ret = flux_sys_ioctl(fd, FLUX_FNET_IOCTL_ADD, (unsigned long)&dev);
	flux_sys_close(fd);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "ioctl FLUX_FNET_IOCTL_ADD failed: %d\n", ret);
		goto out_free;
	}

	ret = flux_fnet_config_dev(ret, &dev);

out_free:
	flux_fnet_free_dev(&dev);
	return ret;
}
