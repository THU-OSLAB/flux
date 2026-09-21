#define _GNU_SOURCE
#define FLUX_FMT "iokd-mlx5: "

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

#include <kernel/asm/fnet.h>
#include <kernel/asm/host_ops.h>
#include <utils/base.h>
#include <utils/log.h>

#include "../iokd.h"
#include "mlx5_prm.h"

#define FLUX_MLX5_PORT 1U
#define FLUX_MLX5_FLOW_TABLE_TYPE 0U
#define FLUX_MLX5_SPEC_WINDOW_OFFSET FLUX_IOK_SHARED_SIZE
#define FLUX_MLX5_SPEC_WINDOW_SIZE (FLUX_IOK_DMA_OFFSET - FLUX_IOK_SHARED_SIZE)
#define FLUX_MLX5_RX_STRIDE 2048U
#define FLUX_MLX5_RX_HEADROOM 256U
#define FLUX_MLX5_CQE_STRIDE 64U
#define FLUX_MLX5_RX_WQE_STRIDE 16U
#define FLUX_MLX5_TX_WQE_STRIDE 64U
#define FLUX_MLX5_BF_OFFSET 0x800U
#define FLUX_MLX5_BF_SIZE 256U
#define FLUX_MLX5_ETHERTYPE_IPV4 0x0800U
#define FLUX_MLX5_ETHERTYPE_ARP 0x0806U
#define FLUX_MLX5_IPPROTO_TCP 6U
#define FLUX_MLX5_TIRC_DIRECT 0U
#define FLUX_MLX5_TIRC_INDIRECT 1U
#define FLUX_MLX5_HASH_TOEPLITZ 2U
#define FLUX_MLX5_HASH_L3_IPV4 0U
#define FLUX_MLX5_HASH_L4_TCP 0U
#define FLUX_MLX5_HASH_IPV4_FIELDS 0x3U
#define FLUX_MLX5_HASH_TCP_FIELDS 0xfU
#define FLUX_MLX5_STATE_WAIT_NS (2ULL * 1000 * 1000 * 1000)

struct flux_mlx5_ring {
	struct mlx5dv_devx_obj *obj;
	uint64_t buf_offset;
	uint64_t dbr_offset;
	uint32_t object_id;
	uint32_t nr_entries;
	uint32_t stride;
};

struct flux_mlx5_queue {
	struct flux_mlx5_ring rx_cq;
	struct flux_mlx5_ring rx_wq;
	struct flux_mlx5_ring tx_cq;
	struct flux_mlx5_ring tx_wq;
	uint32_t rqn;
	uint32_t sqn;
	uint32_t uarn;
	bool uar_allocated;
	bool rq_active;
	bool sq_active;
};

struct flux_mlx5_client {
	struct flux_iokd_client *client;
	struct ibv_pd *pd;
	struct ibv_mr *rx_mr;
	struct ibv_mr *tx_mr;
	struct mlx5dv_devx_umem *umem;
	struct mlx5dv_devx_obj *td;
	struct mlx5dv_devx_obj *tis;
	struct mlx5dv_devx_obj *rqt;
	struct mlx5dv_devx_obj *tcp_tir;
	struct mlx5dv_devx_obj *ipv4_tir;
	struct mlx5dv_devx_obj *arp_tir;
	struct mlx5dv_devx_obj *flow_table;
	struct mlx5dv_devx_obj *tcp_group;
	struct mlx5dv_devx_obj *ipv4_group;
	struct mlx5dv_devx_obj *arp_group;
	struct mlx5dv_devx_obj *tcp_fte;
	struct mlx5dv_devx_obj *ipv4_fte;
	struct mlx5dv_devx_obj *arp_fte;
	struct flux_mlx5_queue *queues;
	struct flux_fnet_mlx5_spec *spec;
	struct flux_fnet_mlx5_shared_state *state;
	void *mem;
	size_t mem_len;
	size_t alloc_offset;
	int memfd;
	uint32_t pdn;
	uint32_t tdn;
	uint32_t tisn;
	uint32_t rqtn;
	uint32_t generation;
	uint32_t nr_queues;
	uint64_t tx_hca_va;
	uint64_t tx_mr_len;
	bool owns_device;
	bool active;
	bool quiesced;
};

struct flux_mlx5_device {
	struct ibv_context *context;
	struct mlx5dv_devx_uar *admin_uar;
	uint32_t eqn;
	off_t bar_offset;
	size_t bar_len;
	int events_fd;
	pthread_mutex_t client_lock;
	bool client_attached;
};

static const uint8_t flux_mlx5_rss_key[40] = {
	0x82, 0x19, 0xfa, 0x80, 0xa4, 0x31, 0x06, 0x59, 0x3e, 0x3f,
	0x9a, 0xac, 0x3d, 0xae, 0xd6, 0xd9, 0xf5, 0xfc, 0x0c, 0x63,
	0x94, 0xbf, 0x8f, 0xde, 0xd2, 0xc5, 0xe2, 0x04, 0xb1, 0xcf,
	0xb1, 0xb1, 0xa1, 0x0d, 0x6d, 0x86, 0xba, 0x61, 0x78, 0xeb,
};

static struct flux_mlx5_device *flux_mlx5_device(void)
{
	return flux_iokd.mlx5_device;
}

static int flux_mlx5_errno(void)
{
	return errno ? -errno : -EIO;
}

static uint64_t flux_mlx5_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int flux_mlx5_memfd_create(const char *name)
{
#ifdef SYS_memfd_create
	return syscall(SYS_memfd_create, name, MFD_CLOEXEC);
#else
	errno = ENOSYS;
	return -1;
#endif
}

static bool flux_mlx5_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	return __builtin_add_overflow(a, b, out);
}

static int flux_mlx5_alloc_region(struct flux_mlx5_client *mlx5, size_t len,
				  size_t alignment, uint64_t *offset_out)
{
	size_t start;

	if (!alignment || (alignment & (alignment - 1)))
		return -EINVAL;
	if (__builtin_add_overflow(mlx5->alloc_offset, alignment - 1, &start))
		return -EOVERFLOW;
	start &= ~(alignment - 1);
	if (start > mlx5->mem_len || len > mlx5->mem_len - start)
		return -ENOSPC;

	memset((uint8_t *)mlx5->mem + start, 0, len);
	*offset_out = start;
	mlx5->alloc_offset = start + len;
	return 0;
}

static int flux_mlx5_alloc_ring(struct flux_mlx5_client *mlx5,
				struct flux_mlx5_ring *ring, uint32_t entries,
				uint32_t stride)
{
	size_t bytes;
	int ret;

	if (!entries || (entries & (entries - 1)) ||
	    __builtin_mul_overflow((size_t)entries, (size_t)stride, &bytes))
		return -EINVAL;
	ret = flux_mlx5_alloc_region(mlx5, bytes, PGSIZE_4KB,
				     &ring->buf_offset);
	if (ret)
		return ret;
	ret = flux_mlx5_alloc_region(mlx5, CACHE_LINE_SIZE, CACHE_LINE_SIZE,
				     &ring->dbr_offset);
	if (ret)
		return ret;
	ring->nr_entries = entries;
	ring->stride = stride;
	return 0;
}

static void flux_mlx5_log_cmd_error(const char *what, const uint32_t *out)
{
	FLUX_LOG(FLUX_LOG_ERR, "%s failed status=%#x syndrome=%#x errno=%d\n",
		 what, DEVX_GET(mbox_out, out, status),
		 DEVX_GET(mbox_out, out, syndrome), errno);
}

static int flux_mlx5_query_vport(struct flux_mlx5_device *dev)
{
	uint32_t in[DEVX_ST_SZ_DW(query_nic_vport_context_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(query_nic_vport_context_out)] = { 0 };
	void *vport;
	uint8_t *mac;
	int ret;

	DEVX_SET(query_nic_vport_context_in, in, opcode,
		 MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
	DEVX_SET(query_nic_vport_context_in, in, allowed_list_type, 0);
	ret = mlx5dv_devx_general_cmd(dev->context, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		flux_mlx5_log_cmd_error("query NIC vport", out);
		return ret < 0 ? ret : -ret;
	}
	vport = DEVX_ADDR_OF(query_nic_vport_context_out, out,
			     nic_vport_context);
	mac = DEVX_ADDR_OF(nic_vport_context, vport, permanent_address) + 2;
	memcpy(flux_iokd.ctrl.host_mac, mac, sizeof(flux_iokd.ctrl.host_mac));
	FLUX_LOG(FLUX_LOG_INFO, "VFIO mlx5 MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return 0;
}

static int flux_mlx5_set_port_mtu(struct flux_mlx5_device *dev, uint16_t mtu)
{
	uint32_t in[DEVX_ST_SZ_DW(pmtu_reg)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(pmtu_reg)] = { 0 };
	int ret;

	DEVX_SET(pmtu_reg, in, admin_mtu, mtu);
	DEVX_SET(pmtu_reg, in, local_port, FLUX_MLX5_PORT);
	ret = mlx5_access_reg(dev->context, in, sizeof(in), out, sizeof(out),
			      MLX5_REG_PMTU, 0, 1);
	if (ret)
		FLUX_LOG(FLUX_LOG_ERR, "failed to set port MTU %u: %d\n", mtu,
			 ret);
	return ret < 0 ? ret : -ret;
}

static int flux_mlx5_setup_vport(struct flux_mlx5_device *dev, uint16_t mtu)
{
	uint32_t in[DEVX_ST_SZ_DW(modify_nic_vport_context_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(modify_nic_vport_context_out)] = { 0 };
	int ret;

	DEVX_SET(modify_nic_vport_context_in, in, opcode,
		 MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
	DEVX_SET(modify_nic_vport_context_in, in, field_select.promisc, 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.promisc_uc,
		 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.promisc_mc,
		 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.promisc_all,
		 1);
	DEVX_SET(modify_nic_vport_context_in, in, field_select.mtu, 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.mtu, mtu);
	ret = mlx5dv_devx_general_cmd(dev->context, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		flux_mlx5_log_cmd_error("configure NIC vport", out);
		return ret < 0 ? ret : -ret;
	}
	return 0;
}

/*
 * export_fd() is the only private entry point in the Flux rdma-core fork.
 * The returned device fd is borrowed from the verbs context.
 */
static int flux_mlx5_vfio_export_bar(struct flux_mlx5_device *dev, int *fd)
{
	export_fd(dev->context, fd, &dev->bar_offset, &dev->bar_len);
	if (*fd < 0 || !dev->bar_len)
		return -ENODEV;
	return 0;
}

int flux_iokd_mlx5_init(void)
{
	struct mlx5dv_vfio_context_attr attr = { 0 };
	struct flux_mlx5_device *dev;
	struct ibv_device **list;
	int exported_fd = -1;
	int ret;

	if (!flux_iokd_cfg.mlx5_external)
		return 0;
	if (flux_iokd.mlx5_device)
		return -EALREADY;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -ENOMEM;
	ret = pthread_mutex_init(&dev->client_lock, NULL);
	if (ret) {
		free(dev);
		return -ret;
	}
	dev->events_fd = -1;
	attr.pci_name = flux_iokd_cfg.nic_pci_addr;
	list = mlx5dv_get_vfio_device_list(&attr);
	if (!list || !list[0] || list[1]) {
		FLUX_LOG(FLUX_LOG_ERR, "expected one mlx5 VFIO device for %s\n",
			 flux_iokd_cfg.nic_pci_addr);
		ret = -ENODEV;
		goto fail_list;
	}
	dev->context = ibv_open_device(list[0]);
	if (!dev->context) {
		ret = flux_mlx5_errno();
		goto fail_list;
	}
	ibv_free_device_list(list);
	list = NULL;

	dev->admin_uar = mlx5dv_devx_alloc_uar(dev->context,
					       MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!dev->admin_uar) {
		ret = flux_mlx5_errno();
		goto fail_context;
	}
	ret = mlx5dv_devx_query_eqn(dev->context, 0, &dev->eqn);
	if (ret) {
		ret = ret < 0 ? ret : -ret;
		goto fail_uar;
	}
	ret = flux_mlx5_vfio_export_bar(dev, &exported_fd);
	if (ret)
		goto fail_uar;
	ret = flux_mlx5_setup_vport(dev, (uint16_t)flux_iokd_cfg.mtu);
	if (ret)
		goto fail_uar;
	ret = flux_mlx5_set_port_mtu(dev, (uint16_t)flux_iokd_cfg.mtu);
	if (ret)
		goto fail_uar;
	ret = flux_mlx5_query_vport(dev);
	if (ret)
		goto fail_uar;
	dev->events_fd = mlx5dv_vfio_get_events_fd(dev->context);

	flux_iokd.mlx5_device = dev;
	flux_iokd.fnet_has_port = true;
	flux_iokd.fnet_is_tap = false;
	flux_iokd.tx_chksum_offload = flux_iokd_cfg.tx_chksum_offload;
	FLUX_LOG(
		FLUX_LOG_INFO,
		"mlx5 external control plane ready bdf=%s eqn=%u bar_off=%#lx bar_len=%#zx\n",
		flux_iokd_cfg.nic_pci_addr, dev->eqn,
		(unsigned long)dev->bar_offset, dev->bar_len);
	return 0;

fail_uar:
	if (dev->admin_uar)
		mlx5dv_devx_free_uar(dev->admin_uar);
fail_context:
	ibv_close_device(dev->context);
fail_list:
	if (list)
		ibv_free_device_list(list);
	pthread_mutex_destroy(&dev->client_lock);
	free(dev);
	return ret;
}

void flux_iokd_mlx5_fini(void)
{
	struct flux_mlx5_device *dev = flux_mlx5_device();

	if (!dev)
		return;
	flux_iokd.mlx5_device = NULL;
	flux_iokd.fnet_has_port = false;
	if (dev->admin_uar)
		mlx5dv_devx_free_uar(dev->admin_uar);
	if (dev->context)
		ibv_close_device(dev->context);
	pthread_mutex_destroy(&dev->client_lock);
	free(dev);
}

static int flux_mlx5_alloc_uar(struct flux_mlx5_device *dev, uint32_t *uarn_out)
{
	uint32_t in[DEVX_ST_SZ_DW(alloc_uar_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(alloc_uar_out)] = { 0 };
	int ret;

	DEVX_SET(alloc_uar_in, in, opcode, MLX5_CMD_OP_ALLOC_UAR);
	ret = mlx5dv_devx_general_cmd(dev->context, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		flux_mlx5_log_cmd_error("allocate UAR", out);
		return ret < 0 ? ret : -ret;
	}
	*uarn_out = DEVX_GET(alloc_uar_out, out, uar);
	return 0;
}

static void flux_mlx5_dealloc_uar(struct flux_mlx5_device *dev, uint32_t uarn)
{
	uint32_t in[DEVX_ST_SZ_DW(dealloc_uar_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(dealloc_uar_out)] = { 0 };

	DEVX_SET(dealloc_uar_in, in, opcode, MLX5_CMD_OP_DEALLOC_UAR);
	DEVX_SET(dealloc_uar_in, in, uar, uarn);
	if (mlx5dv_devx_general_cmd(dev->context, in, sizeof(in), out,
				    sizeof(out)))
		flux_mlx5_log_cmd_error("deallocate UAR", out);
}

static int flux_mlx5_create_td(struct flux_mlx5_device *dev,
			       struct flux_mlx5_client *mlx5)
{
	uint32_t in[DEVX_ST_SZ_DW(alloc_transport_domain_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(alloc_transport_domain_out)] = { 0 };

	DEVX_SET(alloc_transport_domain_in, in, opcode,
		 MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
	mlx5->td = mlx5dv_devx_obj_create(dev->context, in, sizeof(in), out,
					  sizeof(out));
	if (!mlx5->td) {
		flux_mlx5_log_cmd_error("allocate transport domain", out);
		return flux_mlx5_errno();
	}
	mlx5->tdn = DEVX_GET(alloc_transport_domain_out, out, transport_domain);
	return 0;
}

static int flux_mlx5_create_tis(struct flux_mlx5_device *dev,
				struct flux_mlx5_client *mlx5)
{
	uint32_t in[DEVX_ST_SZ_DW(create_tis_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_tis_out)] = { 0 };
	void *ctx;

	DEVX_SET(create_tis_in, in, opcode, MLX5_CMD_OP_CREATE_TIS);
	ctx = DEVX_ADDR_OF(create_tis_in, in, ctx);
	DEVX_SET(tisc, ctx, transport_domain, mlx5->tdn);
	mlx5->tis = mlx5dv_devx_obj_create(dev->context, in, sizeof(in), out,
					   sizeof(out));
	if (!mlx5->tis) {
		flux_mlx5_log_cmd_error("create TIS", out);
		return flux_mlx5_errno();
	}
	mlx5->tisn = DEVX_GET(create_tis_out, out, tisn);
	return 0;
}

static int flux_mlx5_create_cq(struct flux_mlx5_device *dev,
			       struct flux_mlx5_client *mlx5,
			       struct flux_mlx5_ring *cq, uint32_t entries)
{
	uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = { 0 };
	void *cqc;
	int ret;

	ret = flux_mlx5_alloc_ring(mlx5, cq, entries, FLUX_MLX5_CQE_STRIDE);
	if (ret)
		return ret;
	DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
	cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);
	DEVX_SET(cqc, cqc, cqe_sz, 0);
	DEVX_SET(cqc, cqc, cc, 0);
	DEVX_SET(cqc, cqc, oi, 0);
	DEVX_SET(cqc, cqc, cq_period_mode, 0);
	DEVX_SET(cqc, cqc, cqe_comp_en, 0);
	DEVX_SET(cqc, cqc, log_cq_size, __builtin_ctz(entries));
	DEVX_SET(cqc, cqc, uar_page, dev->admin_uar->page_id);
	DEVX_SET(cqc, cqc, cq_period, 0);
	DEVX_SET(cqc, cqc, cq_max_count, 0);
	DEVX_SET(cqc, cqc, c_eqn, dev->eqn);
	DEVX_SET(create_cq_in, in, cq_umem_valid, 1);
	DEVX_SET(create_cq_in, in, cq_umem_id, mlx5->umem->umem_id);
	DEVX_SET64(create_cq_in, in, cq_umem_offset, cq->buf_offset);
	DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
	DEVX_SET(cqc, cqc, dbr_umem_id, mlx5->umem->umem_id);
	DEVX_SET64(cqc, cqc, dbr_addr, cq->dbr_offset);

	cq->obj = mlx5dv_devx_obj_create(dev->context, in, sizeof(in), out,
					 sizeof(out));
	if (!cq->obj) {
		flux_mlx5_log_cmd_error("create CQ", out);
		return flux_mlx5_errno();
	}
	cq->object_id = DEVX_GET(create_cq_out, out, cqn);
	return 0;
}

static void flux_mlx5_fill_wq(struct flux_mlx5_client *mlx5, void *wq,
			      const struct flux_mlx5_ring *ring, uint32_t uarn,
			      bool tx)
{
	DEVX_SET(wq, wq, wq_type, 1);
	if (tx)
		DEVX_SET(wq, wq, uar_page, uarn);
	DEVX_SET(wq, wq, pd, mlx5->pdn);
	DEVX_SET64(wq, wq, dbr_addr, ring->dbr_offset);
	DEVX_SET(wq, wq, log_wq_stride, __builtin_ctz(ring->stride));
	DEVX_SET(wq, wq, log_wq_sz, __builtin_ctz(ring->nr_entries));
	DEVX_SET(wq, wq, dbr_umem_valid, 1);
	DEVX_SET(wq, wq, wq_umem_valid, 1);
	DEVX_SET(wq, wq, dbr_umem_id, mlx5->umem->umem_id);
	DEVX_SET(wq, wq, wq_umem_id, mlx5->umem->umem_id);
	DEVX_SET64(wq, wq, wq_umem_offset, ring->buf_offset);
}

static int flux_mlx5_create_rq(struct flux_mlx5_device *dev,
			       struct flux_mlx5_client *mlx5,
			       struct flux_mlx5_queue *queue)
{
	uint32_t in[DEVX_ST_SZ_DW(create_rq_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_rq_out)] = { 0 };
	void *rqc;
	void *wq;
	int ret;

	ret = flux_mlx5_alloc_ring(mlx5, &queue->rx_wq,
				   FLUX_FNET_MLX5_RX_RING_SIZE,
				   FLUX_MLX5_RX_WQE_STRIDE);
	if (ret)
		return ret;
	DEVX_SET(create_rq_in, in, opcode, MLX5_CMD_OP_CREATE_RQ);
	rqc = DEVX_ADDR_OF(create_rq_in, in, ctx);
	DEVX_SET(rqc, rqc, delay_drop_en, 0);
	DEVX_SET(rqc, rqc, mem_rq_type, 0);
	DEVX_SET(rqc, rqc, state, 0);
	DEVX_SET(rqc, rqc, cqn, queue->rx_cq.object_id);
	wq = DEVX_ADDR_OF(rqc, rqc, wq);
	flux_mlx5_fill_wq(mlx5, wq, &queue->rx_wq, queue->uarn, false);
	queue->rx_wq.obj = mlx5dv_devx_obj_create(dev->context, in, sizeof(in),
						  out, sizeof(out));
	if (!queue->rx_wq.obj) {
		flux_mlx5_log_cmd_error("create RQ", out);
		return flux_mlx5_errno();
	}
	queue->rqn = DEVX_GET(create_rq_out, out, rqn);
	return 0;
}

static int flux_mlx5_create_sq(struct flux_mlx5_device *dev,
			       struct flux_mlx5_client *mlx5,
			       struct flux_mlx5_queue *queue)
{
	uint32_t in[DEVX_ST_SZ_DW(create_sq_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_sq_out)] = { 0 };
	void *sqc;
	void *wq;
	int ret;

	ret = flux_mlx5_alloc_ring(mlx5, &queue->tx_wq,
				   FLUX_FNET_MLX5_TX_RING_SIZE,
				   FLUX_MLX5_TX_WQE_STRIDE);
	if (ret)
		return ret;
	DEVX_SET(create_sq_in, in, opcode, MLX5_CMD_OP_CREATE_SQ);
	sqc = DEVX_ADDR_OF(create_sq_in, in, ctx);
	DEVX_SET(sqc, sqc, cqn, queue->tx_cq.object_id);
	DEVX_SET(sqc, sqc, tis_lst_sz, 1);
	DEVX_SET(sqc, sqc, tis_num_0, mlx5->tisn);
	wq = DEVX_ADDR_OF(sqc, sqc, wq);
	flux_mlx5_fill_wq(mlx5, wq, &queue->tx_wq, queue->uarn, true);
	queue->tx_wq.obj = mlx5dv_devx_obj_create(dev->context, in, sizeof(in),
						  out, sizeof(out));
	if (!queue->tx_wq.obj) {
		flux_mlx5_log_cmd_error("create SQ", out);
		return flux_mlx5_errno();
	}
	queue->sqn = DEVX_GET(create_sq_out, out, sqn);
	return 0;
}

static int flux_mlx5_create_rqt(struct flux_mlx5_device *dev,
				struct flux_mlx5_client *mlx5)
{
	uint32_t out[DEVX_ST_SZ_DW(create_rqt_out)] = { 0 };
	uint32_t nr_entries;
	size_t in_len;
	uint32_t *in;
	void *rqtc;
	uint32_t i;

	nr_entries = mlx5->nr_queues == 1 ?
			     1 :
			     1U << (32 - __builtin_clz(mlx5->nr_queues - 1));
	in_len = DEVX_ST_SZ_BYTES(create_rqt_in) +
		 DEVX_ST_SZ_BYTES(rq_num) * nr_entries;
	in = calloc(1, in_len);
	if (!in)
		return -ENOMEM;
	DEVX_SET(create_rqt_in, in, opcode, MLX5_CMD_OP_CREATE_RQT);
	rqtc = DEVX_ADDR_OF(create_rqt_in, in, rqt_context);
	DEVX_SET(rqtc, rqtc, rqt_max_size, nr_entries);
	DEVX_SET(rqtc, rqtc, rqt_actual_size, nr_entries);
	for (i = 0; i < nr_entries; i++)
		DEVX_SET(rqtc, rqtc, rq_num[i],
			 mlx5->queues[i % mlx5->nr_queues].rqn);
	mlx5->rqt = mlx5dv_devx_obj_create(dev->context, in, in_len, out,
					   sizeof(out));
	free(in);
	if (!mlx5->rqt) {
		flux_mlx5_log_cmd_error("create RQT", out);
		return flux_mlx5_errno();
	}
	mlx5->rqtn = DEVX_GET(create_rqt_out, out, rqtn);
	return 0;
}

static int flux_mlx5_create_tir(struct flux_mlx5_device *dev,
				struct flux_mlx5_client *mlx5,
				struct mlx5dv_devx_obj **obj_out, bool direct,
				bool tcp)
{
	uint32_t in[DEVX_ST_SZ_DW(create_tir_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_tir_out)] = { 0 };
	void *ctx;
	void *hash;

	DEVX_SET(create_tir_in, in, opcode, MLX5_CMD_OP_CREATE_TIR);
	ctx = DEVX_ADDR_OF(create_tir_in, in, ctx);
	DEVX_SET(tirc, ctx, disp_type,
		 direct ? FLUX_MLX5_TIRC_DIRECT : FLUX_MLX5_TIRC_INDIRECT);
	DEVX_SET(tirc, ctx, lro_enable_mask, 0);
	DEVX_SET(tirc, ctx, transport_domain, mlx5->tdn);
	if (direct) {
		DEVX_SET(tirc, ctx, inline_rqn, mlx5->queues[0].rqn);
	} else {
		DEVX_SET(tirc, ctx, indirect_table, mlx5->rqtn);
		DEVX_SET(tirc, ctx, rx_hash_fn, FLUX_MLX5_HASH_TOEPLITZ);
		memcpy(DEVX_ADDR_OF(tirc, ctx, rx_hash_toeplitz_key),
		       flux_mlx5_rss_key, sizeof(flux_mlx5_rss_key));
		hash = DEVX_ADDR_OF(tirc, ctx, rx_hash_field_selector_outer);
		DEVX_SET(rx_hash_field_select, hash, l3_prot_type,
			 FLUX_MLX5_HASH_L3_IPV4);
		if (tcp)
			DEVX_SET(rx_hash_field_select, hash, l4_prot_type,
				 FLUX_MLX5_HASH_L4_TCP);
		DEVX_SET(rx_hash_field_select, hash, selected_fields,
			 tcp ? FLUX_MLX5_HASH_TCP_FIELDS :
			       FLUX_MLX5_HASH_IPV4_FIELDS);
	}
	*obj_out = mlx5dv_devx_obj_create(dev->context, in, sizeof(in), out,
					  sizeof(out));
	if (!*obj_out) {
		flux_mlx5_log_cmd_error("create TIR", out);
		return flux_mlx5_errno();
	}
	return 0;
}

enum flux_mlx5_ingress_kind {
	FLUX_MLX5_INGRESS_TCP,
	FLUX_MLX5_INGRESS_IPV4,
	FLUX_MLX5_INGRESS_ARP,
};

static int flux_mlx5_create_flow_table(struct flux_mlx5_device *dev,
				       struct flux_mlx5_client *mlx5)
{
	uint32_t in[DEVX_ST_SZ_DW(create_flow_table_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_flow_table_out)] = { 0 };
	void *ctx;

	DEVX_SET(create_flow_table_in, in, opcode,
		 MLX5_CMD_OP_CREATE_FLOW_TABLE);
	DEVX_SET(create_flow_table_in, in, table_type,
		 FLUX_MLX5_FLOW_TABLE_TYPE);
	ctx = DEVX_ADDR_OF(create_flow_table_in, in, flow_table_context);
	DEVX_SET(flow_table_context, ctx, table_miss_action, 0);
	DEVX_SET(flow_table_context, ctx, level, 0);
	DEVX_SET(flow_table_context, ctx, log_size, 2);
	mlx5->flow_table = mlx5dv_devx_obj_create(dev->context, in, sizeof(in),
						  out, sizeof(out));
	if (!mlx5->flow_table) {
		flux_mlx5_log_cmd_error("create ingress flow table", out);
		return flux_mlx5_errno();
	}
	return 0;
}

static int flux_mlx5_create_flow_group(struct flux_mlx5_device *dev,
				       struct flux_mlx5_client *mlx5,
				       enum flux_mlx5_ingress_kind kind,
				       uint32_t index,
				       struct mlx5dv_devx_obj **group_out)
{
	uint32_t in[DEVX_ST_SZ_DW(create_flow_group_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(create_flow_group_out)] = { 0 };
	void *criteria;

	DEVX_SET(create_flow_group_in, in, opcode,
		 MLX5_CMD_OP_CREATE_FLOW_GROUP);
	DEVX_SET(create_flow_group_in, in, table_type,
		 FLUX_MLX5_FLOW_TABLE_TYPE);
	DEVX_SET(create_flow_group_in, in, table_id,
		 mlx5_devx_get_obj_id(mlx5->flow_table));
	DEVX_SET(create_flow_group_in, in, match_criteria_enable,
		 DR_MATCHER_CRITERIA_OUTER);
	DEVX_SET(create_flow_group_in, in, start_flow_index, index);
	DEVX_SET(create_flow_group_in, in, end_flow_index, index);
	criteria = DEVX_ADDR_OF(create_flow_group_in, in, match_criteria);
	DEVX_SET(fte_match_param, criteria, outer_headers.ethertype,
		 __devx_mask(16));
	if (kind != FLUX_MLX5_INGRESS_ARP) {
		DEVX_SET(fte_match_param, criteria, outer_headers.ip_version,
			 __devx_mask(4));
		DEVX_SET(fte_match_param, criteria,
			 outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4,
			 __devx_mask(32));
	}
	if (kind == FLUX_MLX5_INGRESS_TCP)
		DEVX_SET(fte_match_param, criteria, outer_headers.ip_protocol,
			 __devx_mask(8));
	*group_out = mlx5dv_devx_obj_create(dev->context, in, sizeof(in), out,
					    sizeof(out));
	if (!*group_out) {
		flux_mlx5_log_cmd_error("create ingress flow group", out);
		return flux_mlx5_errno();
	}
	return 0;
}

static int flux_mlx5_create_fte(struct flux_mlx5_device *dev,
				struct flux_mlx5_client *mlx5,
				enum flux_mlx5_ingress_kind kind,
				uint32_t index, struct mlx5dv_devx_obj *group,
				struct mlx5dv_devx_obj *tir,
				struct mlx5dv_devx_obj **fte_out)
{
	uint32_t in[DEVX_ST_SZ_DW(set_fte_in) + DEVX_ST_SZ_DW(dest_format)] = {
		0
	};
	uint32_t out[DEVX_ST_SZ_DW(set_fte_out)] = { 0 };
	void *ctx;
	void *dest;

	DEVX_SET(set_fte_in, in, opcode, MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY);
	DEVX_SET(set_fte_in, in, table_type, FLUX_MLX5_FLOW_TABLE_TYPE);
	DEVX_SET(set_fte_in, in, table_id,
		 mlx5_devx_get_obj_id(mlx5->flow_table));
	DEVX_SET(set_fte_in, in, flow_index, index);
	ctx = DEVX_ADDR_OF(set_fte_in, in, flow_context);
	DEVX_SET(flow_context, ctx, group_id, mlx5_devx_get_obj_id(group));
	DEVX_SET(flow_context, ctx, action, MLX5_FLOW_CONTEXT_ACTION_FWD_DEST);
	DEVX_SET(flow_context, ctx, destination_list_size, 1);
	if (kind == FLUX_MLX5_INGRESS_ARP) {
		DEVX_SET(flow_context, ctx, match_value.outer_headers.ethertype,
			 FLUX_MLX5_ETHERTYPE_ARP);
	} else {
		DEVX_SET(flow_context, ctx, match_value.outer_headers.ethertype,
			 FLUX_MLX5_ETHERTYPE_IPV4);
		DEVX_SET(flow_context, ctx,
			 match_value.outer_headers.ip_version, 4);
		DEVX_SET(flow_context, ctx,
			 match_value.outer_headers.dst_ipv4_dst_ipv6.ipv4_layout
				 .ipv4,
			 mlx5->client->ip_addr);
		if (kind == FLUX_MLX5_INGRESS_TCP)
			DEVX_SET(flow_context, ctx,
				 match_value.outer_headers.ip_protocol,
				 FLUX_MLX5_IPPROTO_TCP);
	}
	dest = DEVX_ADDR_OF(flow_context, ctx, destination);
	DEVX_SET(dest_format, dest, destination_type, MLX5_FLOW_DEST_TYPE_TIR);
	DEVX_SET(dest_format, dest, destination_id, mlx5_devx_get_obj_id(tir));
	*fte_out = mlx5dv_devx_obj_create(dev->context, in, sizeof(in), out,
					  sizeof(out));
	if (!*fte_out) {
		flux_mlx5_log_cmd_error("create ingress FTE", out);
		return flux_mlx5_errno();
	}
	return 0;
}

static int flux_mlx5_ingress_create(struct flux_mlx5_device *dev,
				    struct flux_mlx5_client *mlx5)
{
	int ret;

	ret = flux_mlx5_create_flow_table(dev, mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_create_flow_group(dev, mlx5, FLUX_MLX5_INGRESS_TCP, 0,
					  &mlx5->tcp_group);
	if (ret)
		return ret;
	ret = flux_mlx5_create_flow_group(dev, mlx5, FLUX_MLX5_INGRESS_IPV4, 1,
					  &mlx5->ipv4_group);
	if (ret)
		return ret;
	ret = flux_mlx5_create_flow_group(dev, mlx5, FLUX_MLX5_INGRESS_ARP, 2,
					  &mlx5->arp_group);
	if (ret)
		return ret;
	ret = flux_mlx5_create_fte(dev, mlx5, FLUX_MLX5_INGRESS_TCP, 0,
				   mlx5->tcp_group, mlx5->tcp_tir,
				   &mlx5->tcp_fte);
	if (ret)
		return ret;
	ret = flux_mlx5_create_fte(dev, mlx5, FLUX_MLX5_INGRESS_IPV4, 1,
				   mlx5->ipv4_group, mlx5->ipv4_tir,
				   &mlx5->ipv4_fte);
	if (ret)
		return ret;
	return flux_mlx5_create_fte(dev, mlx5, FLUX_MLX5_INGRESS_ARP, 2,
				    mlx5->arp_group, mlx5->arp_tir,
				    &mlx5->arp_fte);
}

static void flux_mlx5_destroy_obj(struct mlx5dv_devx_obj **obj,
				  const char *name)
{
	if (!*obj)
		return;
	if (mlx5dv_devx_obj_destroy(*obj))
		FLUX_LOG(FLUX_LOG_WARN, "failed to destroy %s: %s\n", name,
			 strerror(errno));
	*obj = NULL;
}

static void flux_mlx5_ingress_destroy(struct flux_mlx5_client *mlx5)
{
	flux_mlx5_destroy_obj(&mlx5->arp_fte, "ARP FTE");
	flux_mlx5_destroy_obj(&mlx5->ipv4_fte, "IPv4 FTE");
	flux_mlx5_destroy_obj(&mlx5->tcp_fte, "TCP FTE");
	flux_mlx5_destroy_obj(&mlx5->arp_group, "ARP flow group");
	flux_mlx5_destroy_obj(&mlx5->ipv4_group, "IPv4 flow group");
	flux_mlx5_destroy_obj(&mlx5->tcp_group, "TCP flow group");
	flux_mlx5_destroy_obj(&mlx5->flow_table, "ingress flow table");
}

static int flux_mlx5_modify_rq(struct flux_mlx5_queue *queue,
			       uint32_t old_state, uint32_t new_state)
{
	uint32_t in[DEVX_ST_SZ_DW(modify_rq_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(modify_rq_out)] = { 0 };
	void *ctx;
	int ret;

	DEVX_SET(modify_rq_in, in, opcode, MLX5_CMD_OP_MODIFY_RQ);
	DEVX_SET(modify_rq_in, in, rq_state, old_state);
	DEVX_SET(modify_rq_in, in, rqn, queue->rqn);
	ctx = DEVX_ADDR_OF(modify_rq_in, in, ctx);
	DEVX_SET(rqc, ctx, state, new_state);
	ret = mlx5dv_devx_obj_modify(queue->rx_wq.obj, in, sizeof(in), out,
				     sizeof(out));
	if (ret)
		flux_mlx5_log_cmd_error("modify RQ", out);
	return ret < 0 ? ret : -ret;
}

static int flux_mlx5_modify_sq(struct flux_mlx5_queue *queue,
			       uint32_t old_state, uint32_t new_state)
{
	uint32_t in[DEVX_ST_SZ_DW(modify_sq_in)] = { 0 };
	uint32_t out[DEVX_ST_SZ_DW(modify_sq_out)] = { 0 };
	void *ctx;
	int ret;

	DEVX_SET(modify_sq_in, in, opcode, MLX5_CMD_OP_MODIFY_SQ);
	DEVX_SET(modify_sq_in, in, sq_state, old_state);
	DEVX_SET(modify_sq_in, in, sqn, queue->sqn);
	ctx = DEVX_ADDR_OF(modify_sq_in, in, sq_context);
	DEVX_SET(sqc, ctx, state, new_state);
	ret = mlx5dv_devx_obj_modify(queue->tx_wq.obj, in, sizeof(in), out,
				     sizeof(out));
	if (ret)
		flux_mlx5_log_cmd_error("modify SQ", out);
	return ret < 0 ? ret : -ret;
}

static int flux_mlx5_queues_activate(struct flux_mlx5_client *mlx5)
{
	uint32_t i;
	int ret;

	for (i = 0; i < mlx5->nr_queues; i++) {
		ret = flux_mlx5_modify_rq(&mlx5->queues[i], 0, 1);
		if (ret)
			return ret;
		mlx5->queues[i].rq_active = true;
		ret = flux_mlx5_modify_sq(&mlx5->queues[i], 0, 1);
		if (ret)
			return ret;
		mlx5->queues[i].sq_active = true;
	}
	return 0;
}

static void flux_mlx5_queues_error(struct flux_mlx5_client *mlx5)
{
	uint32_t i;

	if (!mlx5->queues)
		return;
	for (i = mlx5->nr_queues; i > 0; i--) {
		struct flux_mlx5_queue *queue = &mlx5->queues[i - 1];

		if (queue->sq_active) {
			flux_mlx5_modify_sq(queue, 1, 3);
			queue->sq_active = false;
		}
		if (queue->rq_active) {
			flux_mlx5_modify_rq(queue, 1, 3);
			queue->rq_active = false;
		}
	}
}

static int flux_mlx5_region_create(struct flux_mlx5_device *dev,
				   struct flux_mlx5_client *mlx5)
{
	size_t spec_len = offsetof(struct flux_fnet_mlx5_spec, queues);
	size_t queue_bytes;
	size_t ring_bytes;
	size_t rx_bytes;
	size_t total;
	uint64_t state_offset;
	uint64_t rx_offset;
	uint32_t rx_count;

	if (__builtin_mul_overflow((size_t)mlx5->nr_queues,
				   sizeof(struct flux_fnet_mlx5_queue_spec),
				   &queue_bytes) ||
	    __builtin_add_overflow(spec_len, queue_bytes, &spec_len))
		return -EOVERFLOW;
	ring_bytes = (size_t)FLUX_FNET_MLX5_RX_RING_SIZE *
			     (FLUX_MLX5_CQE_STRIDE + FLUX_MLX5_RX_WQE_STRIDE) +
		     (size_t)FLUX_FNET_MLX5_TX_RING_SIZE *
			     (FLUX_MLX5_CQE_STRIDE + FLUX_MLX5_TX_WQE_STRIDE) +
		     4 * (PGSIZE_4KB + CACHE_LINE_SIZE);
	if (__builtin_add_overflow(mlx5->nr_queues, 1U, &rx_count) ||
	    __builtin_mul_overflow(rx_count, FLUX_FNET_MLX5_RX_RING_SIZE,
				   &rx_count) ||
	    __builtin_mul_overflow((size_t)rx_count,
				   (size_t)FLUX_MLX5_RX_STRIDE, &rx_bytes) ||
	    __builtin_mul_overflow((size_t)mlx5->nr_queues, ring_bytes,
				   &total) ||
	    __builtin_add_overflow(total, rx_bytes, &total) ||
	    __builtin_add_overflow(total, spec_len + 4 * PGSIZE_2MB, &total))
		return -EOVERFLOW;
	total = align_up(total, PGSIZE_2MB);
	if (total > FLUX_MLX5_SPEC_WINDOW_SIZE)
		return -E2BIG;

	mlx5->memfd = flux_mlx5_memfd_create("flux_mlx5_external");
	if (mlx5->memfd < 0)
		return flux_mlx5_errno();
	if (ftruncate(mlx5->memfd, (off_t)total) < 0)
		return flux_mlx5_errno();
	mlx5->mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED,
			 mlx5->memfd, 0);
	if (mlx5->mem == MAP_FAILED) {
		mlx5->mem = NULL;
		return flux_mlx5_errno();
	}
	mlx5->mem_len = total;
	memset(mlx5->mem, 0, total);
	mlx5->spec = mlx5->mem;
	mlx5->alloc_offset = align_up(spec_len, CACHE_LINE_SIZE);
	if (flux_mlx5_alloc_region(mlx5, sizeof(*mlx5->state), CACHE_LINE_SIZE,
				   &state_offset))
		return -ENOSPC;
	mlx5->state =
		(struct flux_fnet_mlx5_shared_state *)((uint8_t *)mlx5->mem +
						       state_offset);

	mlx5->spec->magic = FLUX_FNET_MLX5_SPEC_MAGIC;
	mlx5->spec->version = FLUX_FNET_MLX5_SPEC_VERSION;
	mlx5->spec->header_len = offsetof(struct flux_fnet_mlx5_spec, queues);
	mlx5->spec->total_len = spec_len;
	mlx5->spec->capabilities =
		FLUX_FNET_MLX5_CAP_REGULAR_RQ | FLUX_FNET_MLX5_CAP_CQE64 |
		FLUX_FNET_MLX5_CAP_FIXED_RQT |
		FLUX_FNET_MLX5_CAP_DIRECT_HCA_VA | FLUX_FNET_MLX5_CAP_VFIO_BAR;
	mlx5->spec->generation = mlx5->generation;
	mlx5->spec->nr_queues = mlx5->nr_queues;
	mlx5->spec->mtu = flux_iokd_cfg.mtu;
	mlx5->spec->memfd_len = total;
	mlx5->spec->state_offset = state_offset;
	mlx5->spec->rx_buf_stride = FLUX_MLX5_RX_STRIDE;
	mlx5->spec->rx_headroom = FLUX_MLX5_RX_HEADROOM;
	mlx5->spec->rx_buf_count = rx_count;
	mlx5->spec->bar_offset = dev->bar_offset;
	mlx5->spec->bar_len = dev->bar_len;
	mlx5->state->generation = mlx5->generation;
	mlx5->state->state = FLUX_FNET_MLX5_STATE_ATTACHED;

	/* Reserve the RX payload region after queue objects have allocated rings. */
	(void)rx_offset;
	return 0;
}

static void flux_mlx5_fill_ring_spec(struct flux_fnet_mlx5_ring_spec *spec,
				     const struct flux_mlx5_ring *ring)
{
	spec->buf_offset = ring->buf_offset;
	spec->dbr_offset = ring->dbr_offset;
	spec->nr_entries = ring->nr_entries;
	spec->stride = ring->stride;
}

static int flux_mlx5_queues_create(struct flux_mlx5_device *dev,
				   struct flux_mlx5_client *mlx5)
{
	uint32_t i;
	int ret;

	ret = flux_mlx5_alloc_uar(dev, &mlx5->queues[0].uarn);
	if (ret)
		return ret;
	mlx5->queues[0].uar_allocated = true;

	for (i = 0; i < mlx5->nr_queues; i++) {
		struct flux_mlx5_queue *queue = &mlx5->queues[i];
		struct flux_fnet_mlx5_queue_spec *spec = &mlx5->spec->queues[i];
		uint64_t bf_end;

		queue->uarn = mlx5->queues[0].uarn;
		ret = flux_mlx5_create_cq(dev, mlx5, &queue->rx_cq,
					  FLUX_FNET_MLX5_RX_RING_SIZE);
		if (ret)
			return ret;
		ret = flux_mlx5_create_cq(dev, mlx5, &queue->tx_cq,
					  FLUX_FNET_MLX5_TX_RING_SIZE);
		if (ret)
			return ret;
		ret = flux_mlx5_create_rq(dev, mlx5, queue);
		if (ret)
			return ret;
		ret = flux_mlx5_create_sq(dev, mlx5, queue);
		if (ret)
			return ret;

		spec->owner_cpu = i;
		spec->cqn_rx = queue->rx_cq.object_id;
		spec->cqn_tx = queue->tx_cq.object_id;
		spec->rqn = queue->rqn;
		spec->sqn = queue->sqn;
		spec->uarn = queue->uarn;
		spec->bar_page_offset = (uint64_t)queue->uarn * PGSIZE_4KB;
		spec->bf_offset =
			FLUX_MLX5_BF_OFFSET + (i & 1U) * 2U * FLUX_MLX5_BF_SIZE;
		spec->bf_size = FLUX_MLX5_BF_SIZE;
		if (flux_mlx5_add_overflow_u64(spec->bar_page_offset,
					       FLUX_MLX5_BF_OFFSET +
						       2 * FLUX_MLX5_BF_SIZE,
					       &bf_end) ||
		    bf_end > dev->bar_len)
			return -ERANGE;
		flux_mlx5_fill_ring_spec(&spec->rx_cq, &queue->rx_cq);
		flux_mlx5_fill_ring_spec(&spec->rx_wq, &queue->rx_wq);
		flux_mlx5_fill_ring_spec(&spec->tx_cq, &queue->tx_cq);
		flux_mlx5_fill_ring_spec(&spec->tx_wq, &queue->tx_wq);
	}
	return 0;
}

static int flux_mlx5_rx_region_register(struct flux_mlx5_client *mlx5)
{
	size_t len;
	uint64_t offset;
	uint64_t hca_va;
	int ret;

	if (__builtin_mul_overflow((size_t)mlx5->spec->rx_buf_count,
				   (size_t)mlx5->spec->rx_buf_stride, &len))
		return -EOVERFLOW;
	ret = flux_mlx5_alloc_region(mlx5, len, PGSIZE_2MB, &offset);
	if (ret)
		return ret;
	hca_va = mlx5->client->window_base + FLUX_MLX5_SPEC_WINDOW_OFFSET;
	if (flux_mlx5_add_overflow_u64(hca_va, offset, &hca_va))
		return -EOVERFLOW;
	mlx5->rx_mr = ibv_reg_mr_iova(mlx5->pd, (uint8_t *)mlx5->mem + offset,
				      len, hca_va, IBV_ACCESS_LOCAL_WRITE);
	if (!mlx5->rx_mr)
		return flux_mlx5_errno();
	mlx5->spec->rx_buf_offset = offset;
	mlx5->spec->rx_buf_len = len;
	mlx5->spec->rx_hca_va = hca_va;
	mlx5->spec->rx_mr_len = len;
	mlx5->spec->rx_lkey = mlx5->rx_mr->lkey;
	return 0;
}

static int flux_mlx5_pd_create(struct flux_mlx5_device *dev,
			       struct flux_mlx5_client *mlx5)
{
	struct mlx5dv_pd pd_out = { 0 };
	struct mlx5dv_obj obj = { 0 };
	int ret;

	mlx5->pd = ibv_alloc_pd(dev->context);
	if (!mlx5->pd)
		return flux_mlx5_errno();
	obj.pd.in = mlx5->pd;
	obj.pd.out = &pd_out;
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (ret)
		return ret < 0 ? ret : -ret;
	mlx5->pdn = pd_out.pdn;
	return 0;
}

static int flux_mlx5_client_objects_create(struct flux_mlx5_device *dev,
					   struct flux_mlx5_client *mlx5)
{
	int ret;

	ret = flux_mlx5_pd_create(dev, mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_region_create(dev, mlx5);
	if (ret)
		return ret;
	mlx5->umem = mlx5dv_devx_umem_reg(
		dev->context, mlx5->mem, mlx5->mem_len, IBV_ACCESS_LOCAL_WRITE);
	if (!mlx5->umem)
		return flux_mlx5_errno();
	ret = flux_mlx5_create_td(dev, mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_create_tis(dev, mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_queues_create(dev, mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_rx_region_register(mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_create_rqt(dev, mlx5);
	if (ret)
		return ret;
	ret = flux_mlx5_create_tir(dev, mlx5, &mlx5->tcp_tir, false, true);
	if (ret)
		return ret;
	ret = flux_mlx5_create_tir(dev, mlx5, &mlx5->ipv4_tir, false, false);
	if (ret)
		return ret;
	ret = flux_mlx5_create_tir(dev, mlx5, &mlx5->arp_tir, true, false);
	if (ret)
		return ret;
	__atomic_store_n(&mlx5->state->state, FLUX_FNET_MLX5_STATE_PREPARED,
			 __ATOMIC_RELEASE);
	return 0;
}

static void flux_mlx5_client_free(struct flux_mlx5_client *mlx5)
{
	struct flux_mlx5_device *dev = flux_mlx5_device();
	uint32_t i;

	if (!mlx5)
		return;
	flux_mlx5_ingress_destroy(mlx5);
	flux_mlx5_queues_error(mlx5);
	flux_mlx5_destroy_obj(&mlx5->arp_tir, "ARP TIR");
	flux_mlx5_destroy_obj(&mlx5->ipv4_tir, "IPv4 TIR");
	flux_mlx5_destroy_obj(&mlx5->tcp_tir, "TCP TIR");
	flux_mlx5_destroy_obj(&mlx5->rqt, "RQT");
	if (mlx5->queues) {
		for (i = mlx5->nr_queues; i > 0; i--) {
			struct flux_mlx5_queue *queue = &mlx5->queues[i - 1];

			flux_mlx5_destroy_obj(&queue->tx_wq.obj, "SQ");
			flux_mlx5_destroy_obj(&queue->rx_wq.obj, "RQ");
			flux_mlx5_destroy_obj(&queue->tx_cq.obj, "TX CQ");
			flux_mlx5_destroy_obj(&queue->rx_cq.obj, "RX CQ");
			if (queue->uar_allocated && dev) {
				flux_mlx5_dealloc_uar(dev, queue->uarn);
				queue->uar_allocated = false;
			}
		}
	}
	flux_mlx5_destroy_obj(&mlx5->tis, "TIS");
	flux_mlx5_destroy_obj(&mlx5->td, "transport domain");
	if (mlx5->tx_mr && ibv_dereg_mr(mlx5->tx_mr))
		FLUX_LOG(FLUX_LOG_WARN, "failed to deregister TX MR: %s\n",
			 strerror(errno));
	mlx5->tx_mr = NULL;
	if (mlx5->rx_mr && ibv_dereg_mr(mlx5->rx_mr))
		FLUX_LOG(FLUX_LOG_WARN, "failed to deregister RX MR: %s\n",
			 strerror(errno));
	mlx5->rx_mr = NULL;
	if (mlx5->umem && mlx5dv_devx_umem_dereg(mlx5->umem))
		FLUX_LOG(FLUX_LOG_WARN, "failed to deregister queue UMEM: %s\n",
			 strerror(errno));
	mlx5->umem = NULL;
	if (mlx5->pd && ibv_dealloc_pd(mlx5->pd))
		FLUX_LOG(FLUX_LOG_WARN, "failed to deallocate PD: %s\n",
			 strerror(errno));
	mlx5->pd = NULL;
	if (mlx5->mem) {
		if (mlx5->state)
			__atomic_store_n(&mlx5->state->state,
					 FLUX_FNET_MLX5_STATE_DEAD,
					 __ATOMIC_RELEASE);
		munmap(mlx5->mem, mlx5->mem_len);
	}
	if (mlx5->memfd >= 0)
		close(mlx5->memfd);
	if (mlx5->owns_device && dev) {
		pthread_mutex_lock(&dev->client_lock);
		dev->client_attached = false;
		pthread_mutex_unlock(&dev->client_lock);
	}
	free(mlx5->queues);
	free(mlx5);
}

int flux_iokd_mlx5_client_prepare(struct flux_iokd_client *client,
				  struct flux_iok_ctrl_mlx5_prepare *reply,
				  int fds[2])
{
	static uint32_t next_generation;
	struct flux_mlx5_device *dev = flux_mlx5_device();
	struct flux_mlx5_client *mlx5;
	int bar_fd = -1;
	int ret;

	if (!dev || !client || !reply || !fds || client->mlx5 ||
	    client->nr_cpus <= 0 ||
	    client->nr_cpus > (int)FLUX_FNET_MLX5_MAX_QUEUES)
		return -EINVAL;
	pthread_mutex_lock(&dev->client_lock);
	if (dev->client_attached) {
		pthread_mutex_unlock(&dev->client_lock);
		return -EBUSY;
	}
	dev->client_attached = true;
	pthread_mutex_unlock(&dev->client_lock);
	mlx5 = calloc(1, sizeof(*mlx5));
	if (!mlx5) {
		pthread_mutex_lock(&dev->client_lock);
		dev->client_attached = false;
		pthread_mutex_unlock(&dev->client_lock);
		return -ENOMEM;
	}
	mlx5->owns_device = true;
	mlx5->memfd = -1;
	mlx5->client = client;
	mlx5->nr_queues = client->nr_cpus;
	mlx5->generation =
		__atomic_add_fetch(&next_generation, 1, __ATOMIC_RELAXED);
	if (!mlx5->generation)
		mlx5->generation = __atomic_add_fetch(&next_generation, 1,
						      __ATOMIC_RELAXED);
	mlx5->queues = calloc(mlx5->nr_queues, sizeof(*mlx5->queues));
	if (!mlx5->queues) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = flux_mlx5_client_objects_create(dev, mlx5);
	if (ret)
		goto fail;

	fds[0] = fcntl(mlx5->memfd, F_DUPFD_CLOEXEC, 3);
	ret = flux_mlx5_vfio_export_bar(dev, &bar_fd);
	if (ret) {
		if (fds[0] >= 0) {
			close(fds[0]);
			fds[0] = -1;
		}
		goto fail;
	}
	fds[1] = bar_fd >= 0 ? fcntl(bar_fd, F_DUPFD_CLOEXEC, 3) : -1;
	if (fds[0] < 0 || fds[1] < 0) {
		ret = flux_mlx5_errno();
		if (fds[0] >= 0)
			close(fds[0]);
		if (fds[1] >= 0)
			close(fds[1]);
		goto fail;
	}

	memset(reply, 0, sizeof(*reply));
	reply->hdr.magic = FLUX_IOK_CTRL_MAGIC;
	reply->hdr.version = FLUX_IOK_CTRL_VERSION;
	reply->hdr.op = FLUX_IOK_CTRL_MLX5_PREPARE;
	reply->hdr.len = sizeof(*reply);
	reply->spec_len = mlx5->spec->total_len;
	reply->dma_hca_va = FLUX_MEMORY_ADDR;
	reply->dma_len = FLUX_IOK_DMA_SIZE;
	client->mlx5 = mlx5;
	FLUX_LOG(FLUX_LOG_INFO,
		 "client=%d mlx5 prepared queues=%u generation=%u mem=%#zx\n",
		 client->id, mlx5->nr_queues, mlx5->generation, mlx5->mem_len);
	return 0;

fail:
	flux_mlx5_client_free(mlx5);
	return ret;
}

static int flux_mlx5_validate_dma_coverage(struct flux_iokd_client *client,
					   uint64_t hca_va, uint64_t len)
{
	struct flux_iokd_dma_map *map;
	uint64_t end;
	uint64_t pos;
	uint64_t window_base = client->window_base + FLUX_IOK_DMA_OFFSET;

	if (!len || hca_va != FLUX_MEMORY_ADDR || len > FLUX_IOK_DMA_SIZE ||
	    flux_mlx5_add_overflow_u64(hca_va, len, &end) ||
	    end > FLUX_MEMORY_ADDR + FLUX_IOK_DMA_SIZE)
		return -EINVAL;
	pos = hca_va;
	while (pos < end) {
		uint64_t covered = pos;

		for (map = client->dma_maps; map; map = map->next) {
			uint64_t map_end;
			uint64_t expected;

			if (flux_mlx5_add_overflow_u64(map->client_addr,
						       map->len, &map_end) ||
			    map->client_addr > pos || map_end <= pos)
				continue;
			expected = window_base +
				   (map->client_addr - FLUX_MEMORY_ADDR);
			if (map->iokd_addr != expected)
				return -ERANGE;
			if (map_end > covered)
				covered = map_end;
		}
		if (covered == pos)
			return -ENXIO;
		pos = covered < end ? covered : end;
	}
	return 0;
}

int flux_iokd_mlx5_dma_map_done(
	struct flux_iokd_client *client,
	const struct flux_iok_ctrl_mlx5_dma_map_done *req)
{
	struct flux_mlx5_client *mlx5 = client ? client->mlx5 : NULL;
	void *local_base;
	int ret;

	if (!mlx5 || !req || mlx5->tx_mr)
		return -EINVAL;
	ret = flux_mlx5_validate_dma_coverage(client, req->hca_va, req->len);
	if (ret) {
		FLUX_LOG(
			FLUX_LOG_ERR,
			"client=%d DMA_MAP_DONE has incomplete range [%#lx,%#lx): %d\n",
			client->id, (unsigned long)req->hca_va,
			(unsigned long)(req->hca_va + req->len), ret);
		return ret;
	}
	local_base = (void *)(client->window_base + FLUX_IOK_DMA_OFFSET);
	mlx5->tx_mr = ibv_reg_mr_iova(mlx5->pd, local_base, req->len,
				      req->hca_va, IBV_ACCESS_LOCAL_WRITE);
	if (!mlx5->tx_mr)
		return flux_mlx5_errno();
	mlx5->tx_hca_va = req->hca_va;
	mlx5->tx_mr_len = req->len;
	mlx5->spec->tx_hca_va = req->hca_va;
	mlx5->spec->tx_mr_len = req->len;
	mlx5->spec->tx_lkey = mlx5->tx_mr->lkey;
	__atomic_store_n(&mlx5->state->state, FLUX_FNET_MLX5_STATE_TX_MR_READY,
			 __ATOMIC_RELEASE);
	FLUX_LOG(FLUX_LOG_INFO,
		 "client=%d TX MR ready hca=%#lx len=%#lx lkey=%#x\n",
		 client->id, (unsigned long)req->hca_va,
		 (unsigned long)req->len, mlx5->tx_mr->lkey);
	return 0;
}

int flux_iokd_mlx5_client_ready(struct flux_iokd_client *client,
				const struct flux_iok_ctrl_mlx5_ready *req)
{
	struct flux_mlx5_device *dev = flux_mlx5_device();
	struct flux_mlx5_client *mlx5 = client ? client->mlx5 : NULL;
	uint64_t expected;
	uint32_t state;
	int ret;

	if (!dev || !mlx5 || !req || mlx5->active || !mlx5->tx_mr)
		return -EINVAL;
	expected = mlx5->nr_queues == 64 ? UINT64_MAX :
					   (1ULL << mlx5->nr_queues) - 1;
	state = __atomic_load_n(&mlx5->state->state, __ATOMIC_ACQUIRE);
	if (req->generation != mlx5->generation ||
	    req->nr_queues != mlx5->nr_queues ||
	    req->ready_queues != expected ||
	    __atomic_load_n(&mlx5->state->ready_queues, __ATOMIC_ACQUIRE) !=
		    expected ||
	    state != FLUX_FNET_MLX5_STATE_FNET_READY)
		return -EAGAIN;
	ret = flux_mlx5_queues_activate(mlx5);
	if (ret)
		goto fail;
	ret = flux_mlx5_ingress_create(dev, mlx5);
	if (ret)
		goto fail;
	mlx5->active = true;
	__atomic_store_n(&mlx5->state->state, FLUX_FNET_MLX5_STATE_ACTIVE,
			 __ATOMIC_RELEASE);
	FLUX_LOG(
		FLUX_LOG_INFO,
		"client=%d mlx5 active queues=%u static IPv4/ARP ingress installed\n",
		client->id, mlx5->nr_queues);
	return 0;

fail:
	mlx5->state->last_error = -ret;
	flux_mlx5_ingress_destroy(mlx5);
	flux_mlx5_queues_error(mlx5);
	__atomic_store_n(&mlx5->state->state, FLUX_FNET_MLX5_STATE_DEAD,
			 __ATOMIC_RELEASE);
	return ret;
}

int flux_iokd_mlx5_client_quiesce(struct flux_iokd_client *client)
{
	struct flux_mlx5_client *mlx5 = client ? client->mlx5 : NULL;
	uint64_t deadline;

	if (!mlx5)
		return 0;
	if (mlx5->quiesced)
		return 0;
	__atomic_store_n(&mlx5->state->state, FLUX_FNET_MLX5_STATE_QUIESCING,
			 __ATOMIC_RELEASE);
	flux_mlx5_ingress_destroy(mlx5);
	deadline = flux_mlx5_now_ns() + FLUX_MLX5_STATE_WAIT_NS;
	while ((__atomic_load_n(&mlx5->state->outstanding_rx,
				__ATOMIC_ACQUIRE) != 0 ||
		__atomic_load_n(&mlx5->state->inflight_tx, __ATOMIC_ACQUIRE) !=
			0) &&
	       flux_mlx5_now_ns() < deadline) {
		struct timespec pause = { .tv_nsec = 1000000 };

		nanosleep(&pause, NULL);
	}
	if (mlx5->state->outstanding_rx || mlx5->state->inflight_tx)
		FLUX_LOG(
			FLUX_LOG_WARN,
			"client=%d quiesce timed out rx=%lu tx=%lu; forcing queues to error\n",
			client->id, (unsigned long)mlx5->state->outstanding_rx,
			(unsigned long)mlx5->state->inflight_tx);
	flux_mlx5_queues_error(mlx5);
	mlx5->active = false;
	mlx5->quiesced = true;
	__atomic_store_n(&mlx5->state->state, FLUX_FNET_MLX5_STATE_DEAD,
			 __ATOMIC_RELEASE);
	return 0;
}

void flux_iokd_mlx5_client_destroy(struct flux_iokd_client *client)
{
	struct flux_mlx5_client *mlx5;

	if (!client)
		return;
	mlx5 = client->mlx5;
	if (!mlx5)
		return;
	flux_iokd_mlx5_client_quiesce(client);
	client->mlx5 = NULL;
	flux_mlx5_client_free(mlx5);
}

bool flux_iokd_mlx5_dma_range_busy(const struct flux_iokd_client *client,
				   uintptr_t addr, size_t len)
{
	const struct flux_mlx5_client *mlx5 = client ? client->mlx5 : NULL;
	uint64_t end;
	uint64_t mr_end;

	if (!mlx5 || !mlx5->tx_mr || !len ||
	    flux_mlx5_add_overflow_u64(addr, len, &end) ||
	    flux_mlx5_add_overflow_u64(mlx5->tx_hca_va, mlx5->tx_mr_len,
				       &mr_end))
		return false;
	return addr < mr_end && end > mlx5->tx_hca_va;
}
