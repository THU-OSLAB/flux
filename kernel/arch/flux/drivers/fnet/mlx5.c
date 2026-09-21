/*
 * mlx5.c - FastNet mlx5 external data plane
 *
 * The CQ/WQ algorithms and barrier order follow Caladan mlx5_rxtx.c.  All
 * mlx5 objects are created by flux-iokd; this file only consumes PRM layouts,
 * registered addresses and a pre-mapped write-combining UAR.
 */

#define pr_fmt(fmt) "<fnet> mlx5: " fmt

#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/log2.h>
#include <linux/preempt.h>
#include <linux/slab.h>

#include "core.h"
#include "fnet.h"
#include "mlx5.h"

enum {
	FNET_MLX5_SND_DBR = 1,
	FNET_MLX5_OPCODE_SEND = 0x0a,
	FNET_MLX5_CQE_REQ = 0,
	FNET_MLX5_CQE_RESP_SEND = 2,
	FNET_MLX5_CQE_INVALID = 15,
	FNET_MLX5_CQE_L3_OK = 1 << 1,
	FNET_MLX5_CQE_L4_OK = 1 << 2,
	FNET_MLX5_CQE_L3_HDR_TYPE_IPV4 = 0,
	FNET_MLX5_WQE_CTRL_CQ_UPDATE = 2 << 2,
	FNET_MLX5_ETH_WQE_L3_CSUM = 1 << 6,
	FNET_MLX5_ETH_WQE_L4_CSUM = 1 << 7,
};

#define FNET_MLX5_REQUIRED_CAPS                                            \
	(FLUX_FNET_MLX5_CAP_REGULAR_RQ | FLUX_FNET_MLX5_CAP_CQE64 |        \
	 FLUX_FNET_MLX5_CAP_FIXED_RQT | FLUX_FNET_MLX5_CAP_DIRECT_HCA_VA | \
	 FLUX_FNET_MLX5_CAP_VFIO_BAR)

#define FNET_MLX5_QUIESCE_TIMEOUT_MS 2000U
#define FNET_MLX5_RX_CACHE_SIZE 32U
#define FNET_MLX5_RX_CACHE_REFILL 16U
#define FNET_MLX5_RX_PREFETCH_STRIDE 2U

static inline void mlx5_to_device_barrier(void)
{
	asm volatile("" ::: "memory");
}

static inline void mlx5_from_device_barrier(void)
{
	asm volatile("lfence" ::: "memory");
}

static inline void mlx5_wc_fence(void)
{
	asm volatile("sfence" ::: "memory");
}

static inline void mlx5_mmio_write64_be(void *addr, __be64 value)
{
	WRITE_ONCE(*(volatile u64 *)addr, (__force u64)value);
}

static inline bool mlx5_range_valid(u64 offset, u64 len, u64 total)
{
	u64 end;

	return !check_add_overflow(offset, len, &end) && end <= total;
}

static inline void *mlx5_mem_ptr(const struct fnet_mlx5 *mlx5, u64 offset,
				 u64 len)
{
	if (!mlx5_range_valid(offset, len, mlx5->mem_len))
		return NULL;
	return (u8 *)mlx5->mem_base + offset;
}

static inline bool mlx5_hca_range_valid(u64 addr, u64 len, u64 base, u64 mr_len)
{
	u64 addr_end, mr_end;

	if (check_add_overflow(addr, len, &addr_end) ||
	    check_add_overflow(base, mr_len, &mr_end))
		return false;
	return addr >= base && addr_end <= mr_end;
}

static inline u8 mlx5_cqe_status(struct fnet_mlx5_cqe64 *cqe, u32 cqe_cnt,
				 u32 head)
{
	u64 parity = head & cqe_cnt;
	u8 op_own = READ_ONCE(cqe->op_own);
	u8 owner = op_own & 1;
	u8 opcode = op_own >> 4;

	return ((owner == !parity) * FNET_MLX5_CQE_INVALID) | opcode;
}

static inline bool mlx5_csum_ok(struct fnet_mlx5_cqe64 *cqe)
{
	return (((cqe->hds_ip_ext &
		  (FNET_MLX5_CQE_L4_OK | FNET_MLX5_CQE_L3_OK)) ==
		 (FNET_MLX5_CQE_L4_OK | FNET_MLX5_CQE_L3_OK)) &
		(((cqe->l4_hdr_type_etc >> 2) & 0x3) ==
		 FNET_MLX5_CQE_L3_HDR_TYPE_IPV4));
}

static inline unsigned int mlx5_nr_inflight_tx(struct fnet_mlx5_txq *txq)
{
	return txq->wq.head - txq->cq.head;
}

static u64 mlx5_count_inflight_tx(struct fnet_mlx5 *mlx5)
{
	u64 count = 0;
	unsigned int i;

	for (i = 0; i < mlx5->nr_queues; i++)
		count += mlx5_nr_inflight_tx(&mlx5->txqs[i]);
	return count;
}

static u32 mlx5_count_upper_owned_rx_locked(struct fnet_mlx5 *mlx5);
static u32 mlx5_publish_upper_owned_rx(struct fnet_mlx5 *mlx5);

void fnet_mlx5_deactivate(struct fnet_netdev *fnet)
{
	struct fnet_mlx5 *mlx5 = fnet ? fnet->mlx5 : NULL;
	unsigned int i;

	if (!mlx5 || READ_ONCE(mlx5->quiescing))
		return;
	WRITE_ONCE(mlx5->quiescing, true);
	WRITE_ONCE(mlx5->active, false);
	WRITE_ONCE(mlx5->state->state, FLUX_FNET_MLX5_STATE_QUIESCING);
	for (i = 0; i < mlx5->nr_queues; i++) {
		WRITE_ONCE(mlx5->rxqs[i].stopped, true);
		WRITE_ONCE(mlx5->txqs[i].stopped, true);
	}
}

static __always_inline bool mlx5_is_active(struct fnet_mlx5 *mlx5)
{
	if (likely(READ_ONCE(mlx5->active)))
		return true;
	if (unlikely(READ_ONCE(mlx5->quiescing)))
		return false;
	if (smp_load_acquire(&mlx5->state->state) !=
	    FLUX_FNET_MLX5_STATE_ACTIVE)
		return false;
	WRITE_ONCE(mlx5->active, true);
	return true;
}

static void mlx5_set_error(struct fnet_mlx5 *mlx5, int error, u8 syndrome)
{
	if (!mlx5 || !mlx5->state)
		return;
	WRITE_ONCE(mlx5->state->last_error, error);
	WRITE_ONCE(mlx5->state->last_syndrome, syndrome);
}

static __cold void mlx5_tx_cqe_error(struct fnet_mlx5_txq *txq,
				     struct fnet_mlx5_cqe64 *cqe, u8 opcode)
{
	struct fnet_mlx5_err_cqe *err = (void *)cqe;
	u8 syndrome = READ_ONCE(err->syndrome);
	u8 vendor = READ_ONCE(err->vendor_err_synd);

	mlx5_set_error(txq->mlx5, -EIO, syndrome);
	txq->stopped = true;
	pr_err_ratelimited("TX CQE opcode %#x syndrome %#x vendor %#x\n",
			   opcode, syndrome, vendor);
}

static __cold void mlx5_rx_cqe_error(struct fnet_mlx5_rxq *rxq,
				     struct fnet_mlx5_cqe64 *cqe, u8 opcode)
{
	u8 syndrome = 0;
	u8 vendor = 0;

	if (cqe) {
		struct fnet_mlx5_err_cqe *err = (void *)cqe;

		syndrome = READ_ONCE(err->syndrome);
		vendor = READ_ONCE(err->vendor_err_synd);
	}
	mlx5_set_error(rxq->mlx5, -EIO, syndrome);
	rxq->stopped = true;
	pr_err_ratelimited("RX CQE opcode %#x syndrome %#x vendor %#x\n",
			   opcode, syndrome, vendor);
}

static int mlx5_validate_ring(const struct flux_fnet_mlx5_ring_spec *ring,
			      u32 min_stride, u64 mem_len)
{
	u64 bytes;

	if (!is_power_of_2(ring->nr_entries) || !ring->nr_entries ||
	    !is_power_of_2(ring->stride) || ring->stride < min_stride)
		return -EINVAL;
	if (check_mul_overflow((u64)ring->nr_entries, (u64)ring->stride,
			       &bytes) ||
	    !mlx5_range_valid(ring->buf_offset, bytes, mem_len) ||
	    !mlx5_range_valid(ring->dbr_offset, 2 * sizeof(u32), mem_len))
		return -EINVAL;
	return 0;
}

static int mlx5_validate_spec(const struct flux_fnet_netdev *arg,
			      const struct flux_fnet_mlx5_spec *spec)
{
	u64 spec_size;
	u64 rx_bytes;
	u64 rx_needed;
	u64 required = FNET_MLX5_REQUIRED_CAPS;
	unsigned int i;

	if (!spec || !arg->mlx5_base || !arg->mlx5_bar)
		return -EINVAL;
	if (spec->magic != FLUX_FNET_MLX5_SPEC_MAGIC ||
	    spec->version != FLUX_FNET_MLX5_SPEC_VERSION ||
	    spec->header_len != offsetof(struct flux_fnet_mlx5_spec, queues))
		return -EPROTO;
	if (!spec->nr_queues || spec->nr_queues != arg->nb_rx_queues ||
	    spec->nr_queues != arg->nb_tx_queues ||
	    spec->nr_queues > min_t(u32, NR_CPUS, FLUX_FNET_MLX5_MAX_QUEUES))
		return -EINVAL;
	if (check_mul_overflow((u64)spec->nr_queues,
			       (u64)sizeof(spec->queues[0]), &spec_size) ||
	    check_add_overflow(spec_size, (u64)spec->header_len, &spec_size) ||
	    spec->total_len != spec_size)
		return -EINVAL;
	if (spec->memfd_len != arg->mlx5_len ||
	    spec->bar_len > arg->mlx5_bar_len ||
	    (spec->capabilities & required) != required)
		return -EINVAL;
	if (spec->mtu != arg->mtu ||
	    !mlx5_range_valid(spec->state_offset,
			      sizeof(struct flux_fnet_mlx5_shared_state),
			      spec->memfd_len) ||
	    !spec->rx_buf_stride ||
	    spec->rx_headroom < sizeof(struct fnet_mlx5_rx_meta) ||
	    spec->rx_headroom >= spec->rx_buf_stride ||
	    spec->rx_buf_stride - spec->rx_headroom < spec->mtu + ETH_HLEN ||
	    !spec->rx_buf_count ||
	    check_mul_overflow((u64)spec->rx_buf_count,
			       (u64)spec->rx_buf_stride, &rx_bytes) ||
	    rx_bytes > spec->rx_buf_len ||
	    !mlx5_range_valid(spec->rx_buf_offset, spec->rx_buf_len,
			      spec->memfd_len))
		return -EINVAL;
	if (!mlx5_hca_range_valid(spec->rx_hca_va, spec->rx_buf_len,
				  spec->rx_hca_va, spec->rx_mr_len) ||
	    !spec->rx_lkey || spec->tx_hca_va != FLUX_MEMORY_ADDR ||
	    !spec->tx_lkey || !spec->tx_mr_len)
		return -EINVAL;

	for (i = 0; i < spec->nr_queues; i++) {
		const struct flux_fnet_mlx5_queue_spec *q = &spec->queues[i];
		u64 bf_end;
		int ret;

		if (q->owner_cpu != i || q->owner_cpu >= NR_CPUS)
			return -EINVAL;
		ret = mlx5_validate_ring(&q->rx_cq,
					 sizeof(struct fnet_mlx5_cqe64),
					 spec->memfd_len);
		if (ret)
			return ret;
		if (q->rx_cq.stride != FNET_MLX5_CQE_SIZE)
			return -EINVAL;
		ret = mlx5_validate_ring(&q->tx_cq,
					 sizeof(struct fnet_mlx5_cqe64),
					 spec->memfd_len);
		if (ret)
			return ret;
		if (q->tx_cq.stride != FNET_MLX5_CQE_SIZE)
			return -EINVAL;
		ret = mlx5_validate_ring(&q->rx_wq,
					 sizeof(struct fnet_mlx5_wqe_data_seg),
					 spec->memfd_len);
		if (ret)
			return ret;
		if (q->rx_wq.stride != sizeof(struct fnet_mlx5_wqe_data_seg))
			return -EINVAL;
		ret = mlx5_validate_ring(&q->tx_wq, 64, spec->memfd_len);
		if (ret)
			return ret;
		if (q->tx_wq.stride != 64 ||
		    q->rx_cq.nr_entries < q->rx_wq.nr_entries ||
		    q->tx_cq.nr_entries < q->tx_wq.nr_entries ||
		    q->rx_wq.nr_entries != spec->queues[0].rx_wq.nr_entries)
			return -EINVAL;
		if (!q->sqn || !q->bf_size || !is_power_of_2(q->bf_size) ||
		    check_add_overflow((u64)q->bar_page_offset,
				       (u64)q->bf_offset, &bf_end) ||
		    check_add_overflow(bf_end, (u64)q->bf_size * 2, &bf_end) ||
		    bf_end > spec->bar_len)
			return -EINVAL;
	}
	if (check_mul_overflow((u64)spec->nr_queues,
			       (u64)spec->queues[0].rx_wq.nr_entries,
			       &rx_needed) ||
	    rx_needed > spec->rx_buf_count)
		return -EINVAL;

	return 0;
}

static int mlx5_init_cq(struct fnet_mlx5_cq *cq, struct fnet_mlx5_cqe64 *cqes,
			u32 count, u32 *dbr)
{
	unsigned int i;

	cq->cqes = cqes;
	cq->cnt = count;
	cq->dbr = dbr;
	cq->head = 0;
	for (i = 0; i < count; i++)
		cqes[i].op_own = (cqes[i].op_own & ~1U) | 1U;
	return 0;
}

static int mlx5_init_wq(struct fnet_mlx5_wq *wq, void *buf, u32 *dbr, u32 count,
			u32 stride)
{
	wq->buf = buf;
	wq->dbr = dbr;
	wq->cnt = count;
	wq->log_stride = ilog2(stride);
	wq->head = 0;
	wq->buffers = kcalloc(count, sizeof(*wq->buffers), GFP_KERNEL);
	return wq->buffers ? 0 : -ENOMEM;
}

static void mlx5_init_tx_segment(struct fnet_mlx5_txq *txq, unsigned int idx,
				 u32 lkey, u32 sqn)
{
	struct fnet_mlx5_wqe_ctrl_seg *ctrl;
	struct fnet_mlx5_wqe_eth_seg *eseg;
	struct fnet_mlx5_wqe_data_seg *dpseg;
	void *segment;
	int size;

	segment = (u8 *)txq->wq.buf + (idx << txq->wq.log_stride);
	ctrl = segment;
	eseg = (void *)(ctrl + 1);
	dpseg = (void *)eseg +
		((offsetof(struct fnet_mlx5_wqe_eth_seg, inline_hdr) +
		  FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE) &
		 ~0xf);
	size = sizeof(*ctrl) / 16 +
	       (offsetof(struct fnet_mlx5_wqe_eth_seg, inline_hdr) +
		FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE) /
		       16 +
	       sizeof(*dpseg) / 16;

	*(u32 *)(segment + 8) = 0;
	ctrl->imm = 0;
	ctrl->fm_ce_se = FNET_MLX5_WQE_CTRL_CQ_UPDATE;
	ctrl->qpn_ds = cpu_to_be32(size | (sqn << 8));
	memset(eseg, 0, sizeof(*eseg));
	eseg->inline_hdr_sz = cpu_to_be16(FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE);
	dpseg->lkey = cpu_to_be32(lkey);
}

static noinline void mlx5_rx_pool_push_slow(struct fnet_mlx5 *mlx5,
					    struct fnet_mlx5_rx_meta *head,
					    struct fnet_mlx5_rx_meta *tail,
					    u32 count)
{
	unsigned long flags;

	spin_lock_irqsave(&mlx5->rx_pool_lock, flags);
	tail->next = mlx5->rx_pool;
	mlx5->rx_pool = head;
	mlx5->rx_pool_count += count;
	spin_unlock_irqrestore(&mlx5->rx_pool_lock, flags);
}

static noinline void mlx5_rx_pool_push_quiescing(struct fnet_mlx5 *mlx5,
						 struct fnet_mlx5_rx_meta *meta)
{
	unsigned long flags;
	u32 owned;

	spin_lock_irqsave(&mlx5->rx_pool_lock, flags);
	meta->next = mlx5->rx_pool;
	mlx5->rx_pool = meta;
	mlx5->rx_pool_count++;
	owned = mlx5_count_upper_owned_rx_locked(mlx5);
	WRITE_ONCE(mlx5->rx_upper_owned, owned);
	WRITE_ONCE(mlx5->state->outstanding_rx, owned);
	spin_unlock_irqrestore(&mlx5->rx_pool_lock, flags);
}

static noinline struct fnet_mlx5_rx_meta *
mlx5_rx_pool_pop_slow(struct fnet_mlx5 *mlx5, struct fnet_mlx5_rx_cache *cache)
{
	struct fnet_mlx5_rx_meta *meta = NULL;
	unsigned long flags;
	u32 count = 0;

	spin_lock_irqsave(&mlx5->rx_pool_lock, flags);
	while (count < FNET_MLX5_RX_CACHE_REFILL && mlx5->rx_pool) {
		meta = mlx5->rx_pool;
		mlx5->rx_pool = meta->next;
		mlx5->rx_pool_count--;
		meta->next = cache->head;
		cache->head = meta;
		cache->count++;
		count++;
	}
	spin_unlock_irqrestore(&mlx5->rx_pool_lock, flags);

	meta = cache->head;
	if (meta) {
		cache->head = meta->next;
		cache->count--;
	}
	return meta;
}

static __always_inline struct fnet_mlx5_rx_meta *
mlx5_rx_alloc(struct fnet_mlx5 *mlx5)
{
	struct fnet_mlx5_rx_cache *cache;
	struct fnet_mlx5_rx_meta *meta;

	preempt_disable();
	cache = this_cpu_ptr(mlx5->rx_caches);
	meta = cache->head;
	if (likely(meta)) {
		cache->head = meta->next;
		cache->count--;
	} else if (likely(READ_ONCE(mlx5->rx_pool))) {
		meta = mlx5_rx_pool_pop_slow(mlx5, cache);
	}
	preempt_enable();
	return meta;
}

static __always_inline void mlx5_rx_release(struct mbuf *m)
{
	struct fnet_mlx5_rx_meta *meta =
		container_of(m, struct fnet_mlx5_rx_meta, mbuf);
	struct fnet_mlx5 *mlx5 = meta->mlx5;
	struct fnet_mlx5_rx_cache *cache;
	struct fnet_mlx5_rx_meta *batch, *tail;
	u32 count;

	/* RCU callbacks and teardown releases use the serialized shared pool. */
	if (unlikely(!in_task() || READ_ONCE(mlx5->quiescing))) {
		if (READ_ONCE(mlx5->quiescing))
			mlx5_rx_pool_push_quiescing(mlx5, meta);
		else
			mlx5_rx_pool_push_slow(mlx5, meta, meta, 1);
		return;
	}

	preempt_disable();
	cache = this_cpu_ptr(mlx5->rx_caches);
	if (likely(cache->count < FNET_MLX5_RX_CACHE_SIZE)) {
		meta->next = cache->head;
		cache->head = meta;
		cache->count++;
		preempt_enable();
		return;
	}
	batch = cache->head;
	tail = batch;
	for (count = 1; count < FNET_MLX5_RX_CACHE_REFILL; count++)
		tail = tail->next;
	cache->head = tail->next;
	cache->count -= count;
	tail->next = NULL;
	meta->next = cache->head;
	cache->head = meta;
	cache->count++;
	preempt_enable();
	mlx5_rx_pool_push_slow(mlx5, batch, tail, count);
}

static struct fnet_mlx5_rx_meta *
mlx5_rx_meta_from_cookie(struct fnet_mlx5 *mlx5, u32 cookie)
{
	struct fnet_mlx5_rx_meta *meta;
	u64 offset;

	if (cookie >= mlx5->spec->rx_buf_count)
		return NULL;
	offset = mlx5->spec->rx_buf_offset +
		 (u64)cookie * mlx5->spec->rx_buf_stride;
	meta = mlx5_mem_ptr(mlx5, offset, mlx5->spec->rx_buf_stride);
	if (!meta)
		return NULL;
	meta->mlx5 = mlx5;
	meta->buf = (u8 *)meta + mlx5->spec->rx_headroom;
	meta->cookie = cookie;
	meta->next = NULL;
	return meta;
}

static void mlx5_post_rx_meta(struct fnet_mlx5 *mlx5, struct fnet_mlx5_rxq *rxq,
			      struct fnet_mlx5_rx_meta *meta)
{
	struct fnet_mlx5_wqe_data_seg *seg;
	u64 hca_addr;
	u32 index;

	index = rxq->wq.head++ & (rxq->wq.cnt - 1);
	seg = (void *)((u8 *)rxq->wq.buf + (index << rxq->wq.log_stride));
	hca_addr = mlx5->spec->rx_hca_va +
		   ((u8 *)meta->buf -
		    ((u8 *)mlx5->mem_base + mlx5->spec->rx_buf_offset));
	seg->addr = cpu_to_be64(hca_addr);
	rxq->wq.buffers[index] = meta;
}

static int mlx5_init_rxq(struct fnet_mlx5 *mlx5, struct fnet_mlx5_rxq *rxq,
			 const struct flux_fnet_mlx5_queue_spec *q)
{
	struct fnet_mlx5_wqe_data_seg *seg;
	unsigned int i;
	int ret;

	rxq->owner_cpu = q->owner_cpu;
	rxq->mlx5 = mlx5;

	ret = mlx5_init_cq(
		&rxq->cq,
		mlx5_mem_ptr(mlx5, q->rx_cq.buf_offset,
			     (u64)q->rx_cq.nr_entries * q->rx_cq.stride),
		q->rx_cq.nr_entries,
		mlx5_mem_ptr(mlx5, q->rx_cq.dbr_offset, 2 * sizeof(u32)));
	if (ret)
		return ret;
	ret = mlx5_init_wq(
		&rxq->wq,
		mlx5_mem_ptr(mlx5, q->rx_wq.buf_offset,
			     (u64)q->rx_wq.nr_entries * q->rx_wq.stride),
		mlx5_mem_ptr(mlx5, q->rx_wq.dbr_offset, 2 * sizeof(u32)),
		q->rx_wq.nr_entries, q->rx_wq.stride);
	if (ret)
		return ret;

	for (i = 0; i < rxq->wq.cnt; i++) {
		struct fnet_mlx5_rx_meta *meta = mlx5_rx_alloc(mlx5);
		u64 max_len =
			mlx5->spec->rx_buf_stride - mlx5->spec->rx_headroom;

		if (!meta)
			return -EINVAL;
		seg = (void *)((u8 *)rxq->wq.buf + (i << rxq->wq.log_stride));
		seg->byte_count = cpu_to_be32(
			min_t(u64, max_len, mlx5->spec->mtu + ETH_HLEN));
		seg->lkey = cpu_to_be32(mlx5->spec->rx_lkey);
		mlx5_post_rx_meta(mlx5, rxq, meta);
	}
	mlx5_to_device_barrier();
	rxq->wq.dbr[0] = cpu_to_be32(rxq->wq.head & 0xffff);
	return 0;
}

static int mlx5_init_txq(struct fnet_mlx5 *mlx5, struct fnet_mlx5_txq *txq,
			 const struct flux_fnet_mlx5_queue_spec *q)
{
	unsigned int i;
	int ret;

	txq->owner_cpu = q->owner_cpu;
	txq->mlx5 = mlx5;
	txq->bf_reg = (u8 *)mlx5->bar_base + q->bar_page_offset + q->bf_offset;
	txq->bf_offset = 0;
	txq->bf_size = q->bf_size;
	ret = mlx5_init_wq(
		&txq->wq,
		mlx5_mem_ptr(mlx5, q->tx_wq.buf_offset,
			     (u64)q->tx_wq.nr_entries * q->tx_wq.stride),
		mlx5_mem_ptr(mlx5, q->tx_wq.dbr_offset, 2 * sizeof(u32)),
		q->tx_wq.nr_entries, q->tx_wq.stride);
	if (ret)
		return ret;
	for (i = 0; i < txq->wq.cnt; i++)
		mlx5_init_tx_segment(txq, i, mlx5->spec->tx_lkey, q->sqn);
	return mlx5_init_cq(
		&txq->cq,
		mlx5_mem_ptr(mlx5, q->tx_cq.buf_offset,
			     (u64)q->tx_cq.nr_entries * q->tx_cq.stride),
		q->tx_cq.nr_entries,
		mlx5_mem_ptr(mlx5, q->tx_cq.dbr_offset, 2 * sizeof(u32)));
}

static int mlx5_init_rx_pool(struct fnet_mlx5 *mlx5)
{
	unsigned int cpu, i;

	mlx5->rx_caches = alloc_percpu(struct fnet_mlx5_rx_cache);
	if (!mlx5->rx_caches)
		return -ENOMEM;
	for_each_possible_cpu(cpu) {
		struct fnet_mlx5_rx_cache *cache =
			per_cpu_ptr(mlx5->rx_caches, cpu);

		cache->head = NULL;
		cache->count = 0;
	}
	spin_lock_init(&mlx5->rx_pool_lock);
	for (i = 0; i < mlx5->spec->rx_buf_count; i++) {
		struct fnet_mlx5_rx_meta *meta =
			mlx5_rx_meta_from_cookie(mlx5, i);

		if (!meta)
			return -EINVAL;
		meta->next = mlx5->rx_pool;
		mlx5->rx_pool = meta;
		mlx5->rx_pool_count++;
	}
	mlx5->rx_pool_total = mlx5->rx_pool_count;
	return 0;
}

static u32 mlx5_count_cached_rx(struct fnet_mlx5 *mlx5)
{
	u32 count = 0;
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		count += per_cpu_ptr(mlx5->rx_caches, cpu)->count;
	return count;
}

static u32 mlx5_count_posted_rx(struct fnet_mlx5 *mlx5)
{
	u32 count = 0;
	unsigned int i, j;

	for (i = 0; i < mlx5->nr_queues; i++) {
		struct fnet_mlx5_wq *wq = &mlx5->rxqs[i].wq;

		for (j = 0; j < wq->cnt; j++)
			count += READ_ONCE(wq->buffers[j]) != NULL;
	}
	return count;
}

static u32 mlx5_count_upper_owned_rx_locked(struct fnet_mlx5 *mlx5)
{
	u32 returned = mlx5->rx_pool_count + mlx5_count_cached_rx(mlx5) +
		       mlx5_count_posted_rx(mlx5);

	if (WARN_ON_ONCE(returned > mlx5->rx_pool_total))
		return mlx5->rx_pool_total;
	return mlx5->rx_pool_total - returned;
}

static u32 mlx5_publish_upper_owned_rx(struct fnet_mlx5 *mlx5)
{
	unsigned long flags;
	u32 owned;

	spin_lock_irqsave(&mlx5->rx_pool_lock, flags);
	owned = mlx5_count_upper_owned_rx_locked(mlx5);
	WRITE_ONCE(mlx5->rx_upper_owned, owned);
	WRITE_ONCE(mlx5->state->outstanding_rx, owned);
	spin_unlock_irqrestore(&mlx5->rx_pool_lock, flags);
	return owned;
}

static int mlx5_gather_tx_completions(struct mbuf **mbufs,
				      struct fnet_mlx5_txq *txq,
				      unsigned int budget)
{
	unsigned int count;

	for (count = 0; count < budget; count++, txq->cq.head++) {
		struct fnet_mlx5_cqe64 *cqe =
			&txq->cq.cqes[txq->cq.head & (txq->cq.cnt - 1)];
		u8 opcode = mlx5_cqe_status(cqe, txq->cq.cnt, txq->cq.head);
		u16 wqe_idx;

		if (opcode == FNET_MLX5_CQE_INVALID)
			break;
		mlx5_from_device_barrier();
		if (unlikely(opcode != FNET_MLX5_CQE_REQ)) {
			mlx5_tx_cqe_error(txq, cqe, opcode);
			break;
		}
		wqe_idx = be16_to_cpu(cqe->wqe_counter) & (txq->wq.cnt - 1);
		mbufs[count] = smp_load_acquire(
			(struct mbuf **)&txq->wq.buffers[wqe_idx]);
		WRITE_ONCE(txq->wq.buffers[wqe_idx], NULL);
	}
	txq->cq.dbr[0] = cpu_to_be32(txq->cq.head & 0xffffff);
	return count;
}

static u8 mlx5_tx_csum_flags(const struct mbuf *m)
{
	u8 flags = m->txflags;

	return ((flags & FLUX_OLFLAG_IP_CHKSUM) ? FNET_MLX5_ETH_WQE_L3_CSUM :
						  0) |
	       ((flags & FLUX_OLFLAG_L3_CHKSUM) ? FNET_MLX5_ETH_WQE_L4_CSUM :
						  0);
}

static void mlx5_release_copied_skb(struct mbuf *m)
{
	struct sk_buff *skb = (void *)(uintptr_t)m->timestamp;

	net_tx_release_mbuf(m);
	dev_kfree_skb_any(skb);
}

bool fnet_mlx5_xmit_skb(struct fnet_cpu *cpu, struct sk_buff *skb)
{
	struct mbuf *m;
	void *dst;

	m = net_tx_alloc_mbuf(0);
	if (unlikely(!m))
		return false;
	if (unlikely(mbuf_tailroom(m) < skb->len)) {
		mbuf_free(m);
		return false;
	}
	dst = mbuf_put(m, skb->len);
	memcpy(dst, skb->data, skb->len);
	m->hash = skb->hash;
	m->txflags = 0;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		if (skb->protocol == htons(ETH_P_IP))
			m->txflags = FLUX_OLFLAG_IPV4 | FLUX_OLFLAG_IP_CHKSUM |
				     FLUX_OLFLAG_L3_CHKSUM;
		else if (skb->protocol == htons(ETH_P_IPV6))
			m->txflags = FLUX_OLFLAG_IPV6 | FLUX_OLFLAG_L3_CHKSUM;
	}
	m->timestamp = (u64)(uintptr_t)skb;
	m->release = mlx5_release_copied_skb;
	if (likely(fnet_mlx5_tx(cpu, m)))
		return true;
	m->timestamp = 0;
	m->release = net_tx_release_mbuf;
	mbuf_free(m);
	return false;
}

bool fnet_mlx5_tx(struct fnet_cpu *cpu, struct mbuf *m)
{
	struct fnet_mlx5 *mlx5 = cpu->fnet->mlx5;
	struct fnet_mlx5_txq *txq;
	struct fnet_mlx5_wqe_ctrl_seg *ctrl;
	struct fnet_mlx5_wqe_eth_seg *eseg;
	struct fnet_mlx5_wqe_data_seg *dpseg;
	struct mbuf *done[FNET_MLX5_SQ_CLEAN_MAX];
	void *segment;
	u64 data_addr;
	u32 idx;
	int i, nr;

	if (unlikely(!mlx5 || !cpu->mlx5.txq || !mlx5_is_active(mlx5)))
		return false;
	txq = cpu->mlx5.txq;
	if (unlikely(txq->stopped ||
		     m->len < FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE))
		return false;
	if (mlx5_nr_inflight_tx(txq) >= FNET_MLX5_SQ_CLEAN_THRESH) {
		nr = mlx5_gather_tx_completions(done, txq,
						FNET_MLX5_SQ_CLEAN_MAX);
		for (i = 0; i < nr; i++)
			if (done[i])
				mbuf_free(done[i]);
		if (unlikely(mlx5_nr_inflight_tx(txq) >= txq->wq.cnt))
			return false;
	}

	data_addr =
		(u64)(uintptr_t)m->data + FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE;

	idx = txq->wq.head & (txq->wq.cnt - 1);
	segment = (u8 *)txq->wq.buf + (idx << txq->wq.log_stride);
	ctrl = segment;
	eseg = (void *)(ctrl + 1);
	dpseg = (void *)eseg +
		((offsetof(struct fnet_mlx5_wqe_eth_seg, inline_hdr) +
		  FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE) &
		 ~0xf);
	ctrl->opmod_idx_opcode = cpu_to_be32(((txq->wq.head & 0xffff) << 8) |
					     FNET_MLX5_OPCODE_SEND);
	eseg->cs_flags = mlx5_tx_csum_flags(m);
	memcpy(eseg->inline_hdr_start, m->data,
	       FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE);
	dpseg->byte_count =
		cpu_to_be32(m->len - FNET_MLX5_ETH_L2_INLINE_HEADER_SIZE);
	dpseg->addr = cpu_to_be64(data_addr);

	smp_store_release((struct mbuf **)&txq->wq.buffers[idx], m);
	txq->wq.head++;
	mlx5_to_device_barrier();
	txq->wq.dbr[FNET_MLX5_SND_DBR] = cpu_to_be32(txq->wq.head & 0xffff);
	mlx5_wc_fence();
	mlx5_mmio_write64_be((u8 *)txq->bf_reg + txq->bf_offset,
			     *(__be64 *)ctrl);
	mlx5_wc_fence();
	txq->bf_offset ^= txq->bf_size;
	return true;
}

static void mlx5_refill_rxq(struct fnet_mlx5 *mlx5, struct fnet_mlx5_rxq *rxq)
{
	unsigned int nr = 0;

	while (wraps_lt(rxq->wq.head, rxq->cq.head + rxq->wq.cnt)) {
		struct fnet_mlx5_rx_meta *meta = mlx5_rx_alloc(mlx5);

		if (!meta)
			break;
		mlx5_post_rx_meta(mlx5, rxq, meta);
		nr++;
	}
	if (!nr)
		return;
	mlx5_to_device_barrier();
	rxq->wq.dbr[0] = cpu_to_be32(rxq->wq.head & 0xffff);
}

static void mlx5_rx_linux_fallback(struct fnet_cpu *cpu,
				   struct fnet_mlx5_rx_meta *meta,
				   unsigned int len, bool csum_ok)
{
	struct sk_buff *skb;

	skb = __netdev_alloc_skb(cpu->fnet->dev, len, GFP_ATOMIC);
	if (likely(skb)) {
		memcpy(skb_put(skb, len), meta->buf, len);
		skb->protocol = eth_type_trans(skb, cpu->fnet->dev);
		skb->ip_summed = csum_ok ? CHECKSUM_UNNECESSARY : CHECKSUM_NONE;
	}
	mlx5_rx_release(&meta->mbuf);
	if (unlikely(!skb))
		return;
	local_bh_disable();
	netif_receive_skb(skb);
	local_bh_enable();
}

static int mlx5_gather_rx(struct fnet_mlx5_rxq *rxq,
			  struct fnet_mlx5_rx_meta **metas, unsigned int budget)
{
	unsigned int count;

	for (count = 0; count < budget; count++) {
		struct fnet_mlx5_cqe64 *cqe =
			&rxq->cq.cqes[rxq->cq.head & (rxq->cq.cnt - 1)];
		struct fnet_mlx5_rx_meta *meta;
		bool csum_ok;
		u32 len;
		u16 wqe_idx;
		u8 opcode = mlx5_cqe_status(cqe, rxq->cq.cnt, rxq->cq.head);

		if (opcode == FNET_MLX5_CQE_INVALID)
			break;
		mlx5_from_device_barrier();
		if (unlikely(opcode != FNET_MLX5_CQE_RESP_SEND)) {
			mlx5_rx_cqe_error(rxq, cqe, opcode);
			break;
		}
		rxq->cq.head++;
		prefetch(&rxq->cq.cqes[rxq->cq.head & (rxq->cq.cnt - 1)]);
		wqe_idx = be16_to_cpu(cqe->wqe_counter) & (rxq->wq.cnt - 1);
		meta = rxq->wq.buffers[wqe_idx];
		rxq->wq.buffers[wqe_idx] = NULL;
		len = be32_to_cpu(cqe->byte_cnt);
		if (unlikely(!meta)) {
			mlx5_rx_cqe_error(rxq, NULL, 0);
			break;
		}
		mbuf_init(&meta->mbuf, meta->buf, len, 0);
		meta->mbuf.len = len;
		csum_ok = mlx5_csum_ok(cqe);
		meta->mbuf.csum_type = csum_ok ? FLUX_CHKSUM_TYPE_UNNECESSARY :
						 FLUX_CHKSUM_TYPE_NEEDED;
		meta->mbuf.release = mlx5_rx_release;
		metas[count] = meta;
	}
	rxq->cq.dbr[0] = cpu_to_be32(rxq->cq.head & 0xffffff);
	return count;
}

static void mlx5_deliver_rx(struct fnet_cpu *cpu,
			    struct fnet_mlx5_rx_meta **metas,
			    unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct fnet_mlx5_rx_meta *meta = metas[i];
		unsigned int len = meta->mbuf.len;

		if (i + FNET_MLX5_RX_PREFETCH_STRIDE < count)
			prefetch(metas[i + FNET_MLX5_RX_PREFETCH_STRIDE]
					 ->mbuf.data);
#ifdef CONFIG_FLUX_FAST_NET
		if (!net_rx_fast_tcp(&meta->mbuf))
			mlx5_rx_linux_fallback(
				cpu, meta, len,
				meta->mbuf.csum_type ==
					FLUX_CHKSUM_TYPE_UNNECESSARY);
#else
		mlx5_rx_linux_fallback(cpu, meta, len,
				       meta->mbuf.csum_type ==
					       FLUX_CHKSUM_TYPE_UNNECESSARY);
#endif
	}
}

int fnet_mlx5_poll(struct fnet_cpu *cpu, int budget)
{
	struct fnet_mlx5_rx_meta *metas[FNET_MLX5_RX_BATCH_SIZE];
	struct fnet_mlx5 *mlx5 = cpu->fnet->mlx5;
	struct fnet_mlx5_rxq *rxq;
	unsigned int count;

	if (unlikely(!mlx5 || !cpu->mlx5.rxq || !mlx5_is_active(mlx5)))
		return 0;
	rxq = cpu->mlx5.rxq;
	if (unlikely(rxq->stopped))
		return 0;
	count = mlx5_gather_rx(rxq, metas,
			       budget ? min_t(unsigned int, budget,
					      FNET_MLX5_RX_BATCH_SIZE) :
					FNET_MLX5_RX_BATCH_SIZE);
	if (unlikely(!count))
		return 0;
	mlx5_refill_rxq(mlx5, rxq);
	mlx5_deliver_rx(cpu, metas, count);
	return count;
}

int fnet_mlx5_init(struct fnet_netdev *fnet, const struct flux_fnet_netdev *arg)
{
	struct flux_fnet_mlx5_spec *spec = arg->mlx5_spec;
	struct fnet_mlx5 *mlx5;
	unsigned int i;
	int ret;

	BUILD_BUG_ON(sizeof(struct fnet_mlx5_cqe64) != FNET_MLX5_CQE_SIZE);
	BUILD_BUG_ON(sizeof(struct fnet_mlx5_err_cqe) != FNET_MLX5_CQE_SIZE);
	BUILD_BUG_ON(sizeof(struct fnet_mlx5_wqe_ctrl_seg) != 16);
	BUILD_BUG_ON(sizeof(struct fnet_mlx5_wqe_eth_seg) != 32);
	BUILD_BUG_ON(sizeof(struct fnet_mlx5_wqe_data_seg) != 16);
	ret = mlx5_validate_spec(arg, spec);
	if (ret)
		return ret;
	mlx5 = kzalloc(sizeof(*mlx5), GFP_KERNEL);
	if (!mlx5)
		return -ENOMEM;
	mlx5->spec = spec;
	mlx5->mem_base = arg->mlx5_base;
	mlx5->mem_len = arg->mlx5_len;
	mlx5->bar_base = arg->mlx5_bar;
	mlx5->bar_len = arg->mlx5_bar_len;
	mlx5->generation = spec->generation;
	mlx5->nr_queues = spec->nr_queues;
	mlx5->state =
		mlx5_mem_ptr(mlx5, spec->state_offset, sizeof(*mlx5->state));
	if (!mlx5->state ||
	    READ_ONCE(mlx5->state->generation) != mlx5->generation ||
	    READ_ONCE(mlx5->state->state) < FLUX_FNET_MLX5_STATE_TX_MR_READY) {
		ret = -EAGAIN;
		goto out_free;
	}
	mlx5->rxqs = kcalloc(mlx5->nr_queues, sizeof(*mlx5->rxqs), GFP_KERNEL);
	mlx5->txqs = kcalloc(mlx5->nr_queues, sizeof(*mlx5->txqs), GFP_KERNEL);
	if (!mlx5->rxqs || !mlx5->txqs) {
		ret = -ENOMEM;
		goto out_free;
	}
	ret = mlx5_init_rx_pool(mlx5);
	if (ret)
		goto out_free;
	for (i = 0; i < mlx5->nr_queues; i++) {
		ret = mlx5_init_rxq(mlx5, &mlx5->rxqs[i], &spec->queues[i]);
		if (ret)
			goto out_free;
		ret = mlx5_init_txq(mlx5, &mlx5->txqs[i], &spec->queues[i]);
		if (ret)
			goto out_free;
	}
	fnet->mlx5 = mlx5;
	for (i = 0; i < mlx5->nr_queues; i++) {
		fnet->cpus[i]->mlx5.rxq = &mlx5->rxqs[i];
		fnet->cpus[i]->mlx5.txq = &mlx5->txqs[i];
	}
	smp_wmb();
	WRITE_ONCE(mlx5->state->last_error, 0);
	WRITE_ONCE(mlx5->state->last_syndrome, 0);
	WRITE_ONCE(mlx5->state->outstanding_rx, 0);
	WRITE_ONCE(mlx5->state->inflight_tx, 0);
	WRITE_ONCE(mlx5->state->ready_queues,
		   mlx5->nr_queues == 64 ? ~0ULL :
					   (1ULL << mlx5->nr_queues) - 1);
	WRITE_ONCE(mlx5->state->state, FLUX_FNET_MLX5_STATE_FNET_READY);
	return 0;

out_free:
	if (mlx5->rxqs)
		for (i = 0; i < mlx5->nr_queues; i++)
			kfree(mlx5->rxqs[i].wq.buffers);
	if (mlx5->txqs)
		for (i = 0; i < mlx5->nr_queues; i++)
			kfree(mlx5->txqs[i].wq.buffers);
	kfree(mlx5->rxqs);
	kfree(mlx5->txqs);
	free_percpu(mlx5->rx_caches);
	kfree(mlx5);
	return ret;
}

void fnet_mlx5_quiesce(struct fnet_netdev *fnet)
{
	struct fnet_mlx5 *mlx5 = fnet ? fnet->mlx5 : NULL;
	unsigned long deadline;
	unsigned int i;

	if (!mlx5)
		return;
	fnet_mlx5_deactivate(fnet);
	/* Flush connection-destroy callbacks that can return an RX mbuf. */
	rcu_barrier();

	deadline = jiffies + msecs_to_jiffies(FNET_MLX5_QUIESCE_TIMEOUT_MS);
	for (i = 0; i < mlx5->nr_queues; i++) {
		struct fnet_mlx5_txq *txq = &mlx5->txqs[i];

		while (mlx5_nr_inflight_tx(txq)) {
			struct mbuf *done[FNET_MLX5_SQ_CLEAN_MAX];
			int j, nr;

			nr = mlx5_gather_tx_completions(done, txq,
							FNET_MLX5_SQ_CLEAN_MAX);
			for (j = 0; j < nr; j++)
				if (done[j])
					mbuf_free(done[j]);
			if (nr)
				continue;
			if (READ_ONCE(mlx5->state->last_error) ||
			    time_after_eq(jiffies, deadline)) {
				mlx5_set_error(mlx5, -ETIMEDOUT, 0);
				break;
			}
			cond_resched();
		}
	}
	mlx5_publish_upper_owned_rx(mlx5);
	WRITE_ONCE(mlx5->state->inflight_tx, mlx5_count_inflight_tx(mlx5));
}

int fnet_mlx5_destroy(struct fnet_netdev *fnet)
{
	struct fnet_mlx5 *mlx5 = fnet ? fnet->mlx5 : NULL;
	u64 inflight;
	unsigned int i;

	if (!mlx5)
		return 0;
	fnet_mlx5_quiesce(fnet);
	for (i = 0; i < mlx5->nr_queues; i++) {
		struct mbuf *done[FNET_MLX5_SQ_CLEAN_MAX];
		int j, nr;

		do {
			nr = mlx5_gather_tx_completions(done, &mlx5->txqs[i],
							FNET_MLX5_SQ_CLEAN_MAX);
			for (j = 0; j < nr; j++)
				if (done[j])
					mbuf_free(done[j]);
			if (!nr && READ_ONCE(mlx5->state->last_error)) {
				pr_warn("queue %u teardown with %u TX buffers still owned\n",
					i, mlx5_nr_inflight_tx(&mlx5->txqs[i]));
				break;
			}
			if (!nr && mlx5_nr_inflight_tx(&mlx5->txqs[i]))
				cond_resched();
		} while (mlx5_nr_inflight_tx(&mlx5->txqs[i]));
	}
	inflight = mlx5_count_inflight_tx(mlx5);
	WRITE_ONCE(mlx5->state->inflight_tx, inflight);
	if (inflight) {
		pr_err("teardown blocked by %llu in-flight TX buffers\n",
		       inflight);
		return -EBUSY;
	}
	rcu_barrier();
	if (mlx5_publish_upper_owned_rx(mlx5)) {
		pr_err("teardown blocked by %u upper-owned RX buffers\n",
		       mlx5->rx_upper_owned);
		return -EBUSY;
	}
	for (i = 0; i < mlx5->nr_queues; i++) {
		fnet->cpus[i]->mlx5.rxq = NULL;
		fnet->cpus[i]->mlx5.txq = NULL;
	}
	for (i = 0; i < mlx5->nr_queues; i++) {
		kfree(mlx5->rxqs[i].wq.buffers);
		kfree(mlx5->txqs[i].wq.buffers);
	}
	WRITE_ONCE(mlx5->state->state, FLUX_FNET_MLX5_STATE_DEAD);
	WRITE_ONCE(mlx5->state->ready_queues, 0);
	fnet->mlx5 = NULL;
	kfree(mlx5->rxqs);
	kfree(mlx5->txqs);
	free_percpu(mlx5->rx_caches);
	kfree(mlx5);
	return 0;
}

static int fnet_mlx5_stop(struct fnet_netdev *fnet)
{
	return fnet_mlx5_destroy(fnet);
}

const struct fnet_backend_ops fnet_mlx5_ops = {
	.start = fnet_mlx5_init,
	.deactivate = fnet_mlx5_deactivate,
	.quiesce = fnet_mlx5_quiesce,
	.stop = fnet_mlx5_stop,
	.rx_poll = fnet_mlx5_poll,
	.xmit_skb = fnet_mlx5_xmit_skb,
	.xmit_mbuf = fnet_mlx5_tx,
};
