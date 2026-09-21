#define FLUX_FMT "iokd-fnet: "

#include <errno.h>
#include <numa.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <kernel/asm/fnet.h>
#include <utils/base.h>
#include <utils/log.h>
#include <utils/memory.h>

#include "../iokd.h"

#define FLUX_IOKD_TAP_VDEV "net_tap0"

static struct rte_eth_conf flux_iokd_port_conf = {
	.rxmode = {
		.mq_mode = RTE_ETH_MQ_RX_RSS,
		.mtu = FLUX_FNET_MTU,
		.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_TCP |
				  RTE_ETH_RSS_NONFRAG_IPV4_UDP,
		},
	},
};
static bool flux_iokd_fnet_port_stopped;

static int flux_iokd_build_socket_mem(char *buf, size_t len)
{
	int node = numa_node_of_cpu(flux_iokd_cfg.cpu);
	int max_node = numa_max_node();
	size_t used = 0;
	int i;

	if (node < 0 || max_node < node)
		return -EINVAL;

	for (i = 0; i <= max_node; i++) {
		int ret = snprintf(
			buf + used, len - used, "%s%d",
			i ? "," : "--socket-mem=", i == node ? 128 : 0);

		if (ret < 0 || (size_t)ret >= len - used)
			return -ENOSPC;
		used += (size_t)ret;
	}

	return 0;
}

static bool flux_iokd_driver_is_tap(const char *driver_name)
{
	return driver_name &&
	       strncmp(driver_name, "net_tap", strlen("net_tap")) == 0;
}

static void flux_iokd_touch_mapping(void *base, size_t len, size_t pgsize)
{
	volatile char *pos;

	for (pos = (volatile char *)base; pos < (volatile char *)base + len;
	     pos += pgsize) {
		WRITE_ONCE(*pos, *pos);
	}
}

static void *flux_iokd_map_shm(mem_key_t key, size_t len, size_t pgsize)
{
	int shmid;
	int flags = IPC_CREAT | IPC_EXCL | 0777;
	void *addr;

	switch (pgsize) {
	case PGSIZE_2MB:
		flags |= SHM_HUGETLB;
#ifdef SHM_HUGE_2MB
		flags |= SHM_HUGE_2MB;
#endif
		break;
	default:
		errno = EINVAL;
		return MAP_FAILED;
	}

	shmid = shmget((key_t)key, len, flags);
	if (shmid == -1)
		return MAP_FAILED;

	addr = shmat(shmid, NULL, 0);
	if (addr == MAP_FAILED)
		return MAP_FAILED;

	return addr;
}

static int flux_iokd_unmap_shm(void *addr)
{
	return shmdt(addr);
}

static void flux_iokd_rx_pool_remove_shm(void)
{
	int shmid;

	if (!flux_iokd.ctrl.rx_shbuf_key)
		return;

	shmid = shmget((key_t)flux_iokd.ctrl.rx_shbuf_key, 0, 0);
	if (shmid >= 0)
		shmctl(shmid, IPC_RMID, NULL);

	flux_iokd.ctrl.rx_shbuf_key = 0;
}

static void flux_iokd_rx_pktmbuf_priv_init(struct rte_mempool *mp, void *opaque,
					   void *obj, unsigned int obj_idx)
{
	struct rx_priv_data *priv = rte_mbuf_to_priv(obj);

	memset(priv, 0, sizeof(*priv));
}

static void flux_iokd_rx_pool_cleanup(void)
{
	if (flux_iokd.ctrl.rx_shbuf && flux_iokd.ctrl.rx_shbuf_dma_mapped) {
		flux_iok_dma_unmap(&flux_iokd.ctrl, flux_iokd.ctrl.rx_shbuf,
				   flux_iokd.ctrl.rx_shbuf_len,
				   flux_iokd.ctrl.rx_shbuf_pgsize, NULL);
		flux_iokd.ctrl.rx_shbuf_dma_mapped = false;
	}

	if (flux_iokd.ctrl.rx_shbuf) {
		flux_iokd_unmap_shm(flux_iokd.ctrl.rx_shbuf);
		flux_iokd.ctrl.rx_shbuf = NULL;
		flux_iokd.ctrl.rx_shbuf_len = 0;
		flux_iokd.ctrl.rx_shbuf_pgsize = 0;
		flux_iokd.ctrl.rx_obj_size = 0;
		flux_iokd.ctrl.rx_obj_off = 0;
	}
	flux_iokd_rx_pool_remove_shm();
}

static struct rte_mempool *flux_iokd_rx_pool_create_in_shm(void)
{
	struct rte_mempool *mp = NULL;
	struct rte_mempool_objsz objsz;
	struct rte_pktmbuf_pool_private mbp_priv = { 0 };
	uint16_t priv_size;
	unsigned int elt_size;
	ssize_t mem_size;
	size_t len, min_chunk_size, align;
	size_t pgsize = PGSIZE_2MB;
	unsigned int pgshift = PGSHIFT_2MB;
	mem_key_t key = 0;
	unsigned int key_attempt;
	int ret;

	priv_size = RTE_ALIGN(sizeof(struct rx_priv_data), RTE_MBUF_PRIV_ALIGN);
	if (priv_size != sizeof(struct rx_priv_data)) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rx priv_size=%u is not aligned (expected=%zu)\n",
			 priv_size, sizeof(struct rx_priv_data));
		return NULL;
	}

	elt_size =
		sizeof(struct rte_mbuf) + priv_size + RTE_MBUF_DEFAULT_BUF_SIZE;
	mbp_priv.mbuf_data_room_size = RTE_MBUF_DEFAULT_BUF_SIZE;
	mbp_priv.mbuf_priv_size = priv_size;

	mp = rte_mempool_create_empty("FLUX_IOKD_RX_POOL",
				      FLUX_FNET_RX_POOL_SIZE, elt_size,
				      FLUX_FNET_RX_POOL_CACHE_SIZE,
				      sizeof(struct rte_pktmbuf_pool_private),
				      rte_socket_id(), 0);
	if (!mp)
		return NULL;

	ret = rte_mempool_set_ops_byname(mp, RTE_MBUF_DEFAULT_MEMPOOL_OPS,
					 NULL);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "set mempool ops failed: %s\n",
			 rte_strerror(rte_errno));
		goto fail;
	}

	rte_pktmbuf_pool_init(mp, &mbp_priv);
	flux_iokd.ctrl.rx_obj_size =
		rte_mempool_calc_obj_size(elt_size, 0, &objsz);
	flux_iokd.ctrl.rx_obj_off = objsz.header_size;
	if (flux_iokd.ctrl.rx_obj_size !=
		    mp->header_size + mp->elt_size + mp->trailer_size ||
	    flux_iokd.ctrl.rx_obj_off != mp->header_size) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"rx obj geometry mismatch calc=(size=%zu off=%zu) mp=(size=%u off=%u)\n",
			flux_iokd.ctrl.rx_obj_size, flux_iokd.ctrl.rx_obj_off,
			mp->header_size + mp->elt_size + mp->trailer_size,
			mp->header_size);
		goto fail;
	}

#ifndef SHM_HUGETLB
	FLUX_LOG(FLUX_LOG_ERR, "SHM_HUGETLB is required for shared rx pool\n");
	goto fail;
#endif

	mem_size = rte_mempool_ops_calc_mem_size(
		mp, FLUX_FNET_RX_POOL_SIZE, pgshift, &min_chunk_size, &align);
	if (mem_size < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "calc rx pool mem size failed: %s\n",
			 rte_strerror(rte_errno));
		goto fail;
	}
	len = (size_t)mem_size;
	len = align_up(len, pgsize);

	for (key_attempt = 0; key_attempt < 1024; key_attempt++) {
		key = ((mem_key_t)getpid() << 10) ^ 0x46525800U ^ key_attempt;
		flux_iokd.ctrl.rx_shbuf = flux_iokd_map_shm(key, len, pgsize);
		if (flux_iokd.ctrl.rx_shbuf != MAP_FAILED)
			break;
		if (errno != EEXIST)
			break;
	}
	if (flux_iokd.ctrl.rx_shbuf == MAP_FAILED) {
		flux_iokd.ctrl.rx_shbuf = NULL;
		FLUX_LOG(FLUX_LOG_ERR,
			 "map shared rx shm failed pgsize=%zu len=%zu: %s\n",
			 pgsize, len, strerror(errno));
		goto fail;
	}

	flux_iokd_touch_mapping(flux_iokd.ctrl.rx_shbuf, len, pgsize);
	flux_iokd.ctrl.rx_shbuf_len = len;
	flux_iokd.ctrl.rx_shbuf_key = key;
	flux_iokd.ctrl.rx_shbuf_pgsize = pgsize;
	FLUX_LOG(FLUX_LOG_INFO,
		 "mapped shared rx shm key=%u base=%p len=%zu pgsize=%zu\n",
		 key, flux_iokd.ctrl.rx_shbuf, len, pgsize);

	ret = flux_iok_dma_map(&flux_iokd.ctrl, flux_iokd.ctrl.rx_shbuf, len,
			       pgsize, NULL);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "shared rx dma map failed pgsize=%zu len=%zu ret=%d\n",
			 pgsize, len, ret);
		goto fail;
	}
	flux_iokd.ctrl.rx_shbuf_dma_mapped = true;

	ret = rte_mempool_populate_virt(mp, flux_iokd.ctrl.rx_shbuf, len,
					pgsize, NULL, NULL);
	if (ret < 0) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"populate shared rx mempool failed pgsize=%zu len=%zu ret=%d dpdk=%s\n",
			pgsize, len, ret, rte_strerror(rte_errno));
		goto fail;
	}

	rte_mempool_obj_iter(mp, rte_pktmbuf_init, NULL);
	rte_mempool_obj_iter(mp, flux_iokd_rx_pktmbuf_priv_init, NULL);
	FLUX_LOG(
		FLUX_LOG_INFO,
		"shared rx pool ready key=%u len=%zu pgsize=%zu obj_size=%zu obj_off=%zu elt=%u header=%u trailer=%u priv=%u\n",
		flux_iokd.ctrl.rx_shbuf_key, len, pgsize,
		flux_iokd.ctrl.rx_obj_size, flux_iokd.ctrl.rx_obj_off,
		mp->elt_size, mp->header_size, mp->trailer_size, priv_size);
	return mp;

fail:
	if (mp)
		rte_mempool_free(mp);
	if (flux_iokd.ctrl.rx_shbuf || flux_iokd.ctrl.rx_shbuf_key)
		flux_iokd_rx_pool_cleanup();
	return NULL;
}

static int flux_iokd_eal_init(void)
{
	char *argv[16];
	char cpu_buf[16];
	char socket_mem_buf[128];
	int argc = 0;
	int ret;
	int i;

	argv[argc++] = "./flux-iokd";
	argv[argc++] = "-l";
	snprintf(cpu_buf, sizeof(cpu_buf), "%d", flux_iokd_cfg.cpu);
	argv[argc++] = cpu_buf;
	argv[argc++] = "--file-prefix=" FLUX_IOKD_NAME_PREFIX;
	argv[argc++] = "--proc-type=primary";
	argv[argc++] = "--no-telemetry";
	argv[argc++] = "--log-level=error";

	if (flux_iokd_cfg.no_network) {
		argv[argc++] = "--no-huge";
		argv[argc++] = "--no-pci";
	} else if (flux_iokd_cfg.nic_pci_addr &&
		   flux_iokd_cfg.nic_pci_addr[0]) {
		ret = flux_iokd_build_socket_mem(socket_mem_buf,
						 sizeof(socket_mem_buf));
		if (ret)
			return ret;
		argv[argc++] = socket_mem_buf;
		argv[argc++] = "--allow";
		argv[argc++] = flux_iokd_cfg.nic_pci_addr;
	} else {
		ret = flux_iokd_build_socket_mem(socket_mem_buf,
						 sizeof(socket_mem_buf));
		if (ret)
			return ret;
		argv[argc++] = socket_mem_buf;
		argv[argc++] = "--no-pci";
		argv[argc++] = "--vdev=" FLUX_IOKD_TAP_VDEV;
	}

	FLUX_LOG(FLUX_LOG_INFO, "initializing DPDK EAL on cpu %d\n",
		 flux_iokd_cfg.cpu);
	for (i = 0; i < argc; i++)
		FLUX_LOG(FLUX_LOG_INFO, "  eal argv[%d] = %s\n", i, argv[i]);

	ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "rte_eal_init failed: %s\n",
			 rte_strerror(rte_errno));
		return ret;
	}

	FLUX_LOG(FLUX_LOG_INFO, "DPDK EAL initialized: ports=%u iova=%s\n",
		 rte_eth_dev_count_avail(),
		 rte_eal_iova_mode() == RTE_IOVA_PA ? "PA" : "VA");
	return ret;
}

static int flux_iokd_port_init(void)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_rxconf *rxconf;
	struct rte_eth_txconf *txconf;
	uint16_t nb_rxd = FLUX_FNET_RX_RING_SIZE;
	uint16_t nb_txd = FLUX_FNET_TX_RING_SIZE;
	int ret;

	FLUX_LOG(FLUX_LOG_INFO, "probing FNET port %u\n", FLUX_FNET_PORT);

	if (!rte_eth_dev_is_valid_port(FLUX_FNET_PORT)) {
		FLUX_LOG(FLUX_LOG_ERR, "DPDK port %u is not available\n",
			 FLUX_FNET_PORT);
		return -EINVAL;
	}

	ret = rte_eth_dev_info_get(FLUX_FNET_PORT, &dev_info);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "rte_eth_dev_info_get failed: %d (%s)\n",
			 ret, rte_strerror(-ret));
		return ret;
	}

	flux_iokd.ctrl.port = FLUX_FNET_PORT;
	flux_iokd.ctrl.dev = dev_info.device;
	flux_iokd.ctrl.iova_mode_pa = rte_eal_iova_mode() != RTE_IOVA_VA;
	flux_iokd.fnet_is_tap = flux_iokd_driver_is_tap(dev_info.driver_name);
	flux_iokd.fnet_has_port = true;
	flux_iokd.tx_chksum_offload = flux_iokd_cfg.tx_chksum_offload;
	if (flux_iokd.fnet_is_tap) {
		/*
		 * DPDK TAP emulates TX offloads by cloning buffers from the TX
		 * mempool. That does not work with our external-buffer path.
		 */
		flux_iokd.tx_chksum_offload = false;
	}

	flux_iokd.rx_mempool = flux_iokd_rx_pool_create_in_shm();
	if (!flux_iokd.rx_mempool) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to create shared rx mempool: %s\n",
			 rte_strerror(rte_errno));
		return -ENOMEM;
	}

	FLUX_LOG(
		FLUX_LOG_INFO,
		"using DPDK port %u driver=%s socket=%d requested mtu=%u tx_csum=%d\n",
		FLUX_FNET_PORT,
		dev_info.driver_name ? dev_info.driver_name : "(unknown)",
		rte_eth_dev_socket_id(FLUX_FNET_PORT), flux_iokd_cfg.mtu,
		flux_iokd.tx_chksum_offload);

	flux_iokd_port_conf.txmode.offloads =
		flux_iokd.tx_chksum_offload ? (RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
					       RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
					       RTE_ETH_TX_OFFLOAD_TCP_CKSUM) :
					      0;

	ret = rte_eth_dev_configure(FLUX_FNET_PORT, 1, 1, &flux_iokd_port_conf);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rte_eth_dev_configure failed: %d (%s)\n", ret,
			 rte_strerror(-ret));
		return ret;
	}
	FLUX_LOG(FLUX_LOG_INFO, "configured ethernet device\n");

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(FLUX_FNET_PORT, &nb_rxd,
					       &nb_txd);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rte_eth_dev_adjust_nb_rx_tx_desc failed: %d (%s)\n",
			 ret, rte_strerror(-ret));
		return ret;
	}
	FLUX_LOG(FLUX_LOG_INFO, "descriptor counts: rx=%u tx=%u\n", nb_rxd,
		 nb_txd);

	rxconf = &dev_info.default_rxconf;
	ret = rte_eth_rx_queue_setup(FLUX_FNET_PORT, 0, nb_rxd,
				     rte_eth_dev_socket_id(FLUX_FNET_PORT),
				     rxconf, flux_iokd.rx_mempool);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rte_eth_rx_queue_setup failed: %d (%s)\n", ret,
			 rte_strerror(-ret));
		return ret;
	}
	FLUX_LOG(FLUX_LOG_INFO, "rx queue ready\n");

	txconf = &dev_info.default_txconf;
	ret = rte_eth_tx_queue_setup(FLUX_FNET_PORT, 0, nb_txd,
				     rte_eth_dev_socket_id(FLUX_FNET_PORT),
				     txconf);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "rte_eth_tx_queue_setup failed: %d (%s)\n", ret,
			 rte_strerror(-ret));
		return ret;
	}
	FLUX_LOG(FLUX_LOG_INFO, "tx queue ready\n");

	ret = rte_eth_dev_start(FLUX_FNET_PORT);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "rte_eth_dev_start failed: %d (%s)\n",
			 ret, rte_strerror(-ret));
		return ret;
	}
	FLUX_LOG(FLUX_LOG_INFO, "ethernet device started\n");

	rte_eth_promiscuous_enable(FLUX_FNET_PORT);
	rte_eth_macaddr_get(FLUX_FNET_PORT,
			    (struct rte_ether_addr *)flux_iokd.ctrl.host_mac);
	if (flux_iokd.fnet_is_tap)
		flux_iokd.ctrl.host_mac[5]++;
	FLUX_LOG(FLUX_LOG_INFO,
		 "port %u mac %02x:%02x:%02x:%02x:%02x:%02x iova=%s\n",
		 FLUX_FNET_PORT, flux_iokd.ctrl.host_mac[0],
		 flux_iokd.ctrl.host_mac[1], flux_iokd.ctrl.host_mac[2],
		 flux_iokd.ctrl.host_mac[3], flux_iokd.ctrl.host_mac[4],
		 flux_iokd.ctrl.host_mac[5],
		 flux_iokd.ctrl.iova_mode_pa ? "PA" : "VA");
	return 0;
}

int flux_iokd_fnet_init(void)
{
	int ret;
	size_t data_room_size =
		flux_iokd_cfg.tx_copy ? RTE_MBUF_DEFAULT_BUF_SIZE : 0;

	flux_iokd_fnet_port_stopped = false;
	flux_iokd.fnet_has_port = false;
	flux_iokd.fnet_is_tap = false;
	flux_iokd.tx_chksum_offload = false;
	if (flux_iokd_cfg.mlx5_external)
		return flux_iokd_mlx5_init();

	if (flux_iokd_eal_init() < 0)
		return -1;

	ret = flux_iokd_rx_clients_init();
	if (ret < 0) {
		rte_eal_cleanup();
		return ret;
	}

	flux_iokd.tx_mempool = flux_iokd_tx_pool_create(data_room_size);
	if (!flux_iokd.tx_mempool) {
		FLUX_LOG(FLUX_LOG_ERR, "flux_iokd_tx_pool_create failed\n");
		flux_iokd_rx_clients_fini();
		rte_eal_cleanup();
		return -1;
	}

	if (flux_iokd_cfg.no_network) {
		FLUX_LOG(
			FLUX_LOG_INFO,
			"network dataplane disabled; starting control-only iokd\n");
		return 0;
	}

	ret = flux_iokd_port_init();
	if (ret < 0) {
		flux_iokd_fnet_fini();
		return ret;
	}

	return 0;
}

void flux_iokd_fnet_stop(void)
{
	if (flux_iokd_cfg.mlx5_external)
		return;
	flux_iokd_tx_shutdown_flush();
	if (!flux_iokd.fnet_has_port ||
	    !rte_eth_dev_is_valid_port(FLUX_FNET_PORT))
		return;
	if (flux_iokd_fnet_port_stopped)
		return;

	FLUX_LOG(FLUX_LOG_INFO, "stopping ethernet device port=%u\n",
		 FLUX_FNET_PORT);
	rte_eth_dev_stop(FLUX_FNET_PORT);
	flux_iokd_fnet_port_stopped = true;
}

void flux_iokd_fnet_fini(void)
{
	int ret;

	if (flux_iokd_cfg.mlx5_external) {
		flux_iokd_mlx5_fini();
		return;
	}

	flux_iokd_fnet_stop();
	if (flux_iokd.fnet_has_port &&
	    rte_eth_dev_is_valid_port(FLUX_FNET_PORT))
		rte_eth_dev_close(FLUX_FNET_PORT);
	if (flux_iokd.tx_mempool)
		rte_mempool_free(flux_iokd.tx_mempool);
	if (flux_iokd.rx_mempool)
		rte_mempool_free(flux_iokd.rx_mempool);
	flux_iokd_rx_clients_fini();
	flux_iokd.tx_mempool = NULL;
	flux_iokd.rx_mempool = NULL;
	flux_iokd_fnet_port_stopped = false;
	flux_iokd.fnet_has_port = false;
	flux_iokd.fnet_is_tap = false;
	flux_iokd.tx_chksum_offload = false;
	flux_iokd_rx_pool_cleanup();

	ret = rte_eal_cleanup();
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_WARN, "rte_eal_cleanup failed: %d (%s)\n",
			 ret, rte_strerror(rte_errno));
	}
}
