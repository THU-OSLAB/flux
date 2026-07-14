#define pr_fmt(fmt) "spdk: " fmt

#include <linux/module.h>
#include <linux/blk-mq.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/preempt.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h> /* For struct sched_param */
#include <linux/hdreg.h>
#include <linux/blkdev.h>
#include <linux/idr.h>
#include <linux/sched/signal.h>
#include <linux/rcupdate.h>
#include <linux/fdtable.h>
#include <asm/host_ops.h>

#include "spdk.h"

static int spdk_bdev_major;

static DEFINE_IDR(spdk_index_idr);

static DEFINE_MUTEX(spdk_mutex);

static int spdk_poll_thread(void *);
static void spdk_process_request(struct request *rq, struct spdk_poll_ctx *ctx);
static void spdk_exit(void);

static void reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct spdk_cmd *cmd = (struct spdk_cmd *)ref;
	struct request *req = cmd->req;
	struct req_iterator *iter = &cmd->iter;
	struct bio_vec bvec;

	cmd->iov_offset = sgl_offset;

	BUG_ON(!req->bio);

	for (iter->bio = req->bio; iter->bio; iter->bio = iter->bio->bi_next) {
		for (iter->iter = iter->bio->bi_iter;
		     iter->iter.bi_size &&
		     ((bvec = bio_iter_iovec(iter->bio, iter->iter)), 1);
		     bio_advance_iter(iter->bio, &iter->iter, bvec.bv_len)) {
			if (cmd->iov_offset < bvec.bv_len) {
				return;
			}
			cmd->iov_offset -= bvec.bv_len;
		}
	}
}

static int next_sgl(void *ref, void **address, uint32_t *length)
{
	struct spdk_cmd *cmd = (struct spdk_cmd *)ref;
	struct request *req = cmd->req;
	struct req_iterator *iter = &cmd->iter;
	struct bio_vec bvec;

	BUG_ON(!req->bio);

	if (!iter->iter.bi_size) {
		BUG_ON(!iter->bio);
		iter->bio = iter->bio->bi_next;
		iter->iter = iter->bio->bi_iter;

		BUG_ON(!iter->iter.bi_size);
	}

	bvec = bio_iter_iovec(iter->bio, iter->iter);
	BUG_ON(cmd->iov_offset > bvec.bv_len);

	*address = lowmem_page_address(bvec.bv_page) + bvec.bv_offset +
		   cmd->iov_offset;
	*length = bvec.bv_len - cmd->iov_offset;

	bio_advance_iter(iter->bio, &iter->iter, bvec.bv_len);
	cmd->iov_offset = 0;

	return 0;
}

static void spdk_read_copy_completion_cb(void *ctx,
					 const struct spdk_nvme_cpl *cpl)
{
	struct spdk_cmd *cmd = (struct spdk_cmd *)ctx;
	struct bio_vec bvec;
	struct request *req = cmd->req;
	struct req_iterator iter;
	char *p = (char *)cmd->spdk_buf;

	rq_for_each_segment(bvec, req, iter) {
		memcpy(page_address(bvec.bv_page) + bvec.bv_offset, p,
		       bvec.bv_len);
		p += bvec.bv_len;
	}

	flux_spdk_ops_mempool_put(flux_spdk->dma_mempool, cmd->spdk_buf);
	cmd->spdk_buf = NULL;

	BUG_ON(flux_spdk_ops_cpl_is_error(cpl));

	cmd->poll_ctx->qlen--;
	blk_mq_end_request(req, BLK_STS_OK);
}

static void spdk_read_zerocopy_completion_cb(void *ctx,
					     const struct spdk_nvme_cpl *cpl)
{
	struct spdk_cmd *cmd = (struct spdk_cmd *)ctx;
	struct request *req = cmd->req;

	BUG_ON(flux_spdk_ops_cpl_is_error(cpl));

	cmd->poll_ctx->qlen--;
	blk_mq_end_request(req, BLK_STS_OK);
}

static int spdk_read_zerocopy(struct spdk_cmd *cmd, struct request *req,
			      uint64_t lba, uint32_t lba_count)
{
	struct spdk_poll_ctx *ctx = cmd->poll_ctx;

	return flux_spdk_ops_ns_cmd_readv(ctx->dev->ns_entry.ns, ctx->qpair, lba,
					 lba_count,
					 spdk_read_zerocopy_completion_cb, cmd,
					 0, reset_sgl, next_sgl);
}

static int spdk_read_copy(struct spdk_cmd *cmd, struct request *req,
			  uint64_t lba, uint32_t lba_count)
{
	int rc;

	cmd->spdk_buf = flux_spdk_ops_mempool_get(flux_spdk->dma_mempool);
	rc = flux_spdk_ops_ns_cmd_read(cmd->poll_ctx->dev->ns_entry.ns,
				      cmd->poll_ctx->qpair, cmd->spdk_buf, lba,
				      lba_count, spdk_read_copy_completion_cb,
				      cmd, 0);
	if (unlikely(rc < 0)) {
		flux_spdk_ops_mempool_put(flux_spdk->dma_mempool, cmd->spdk_buf);
		cmd->spdk_buf = NULL;
		return rc;
	}

	return rc;
}
static __always_inline int spdk_read(struct spdk_cmd *cmd, struct request *req,
				     uint64_t lba, uint32_t lba_count)
{
	if (flux_spdk->zero_copy) {
		return spdk_read_zerocopy(cmd, req, lba, lba_count);
	} else {
		return spdk_read_copy(cmd, req, lba, lba_count);
	}
}

static void spdk_write_completion_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_cmd *cmd = (struct spdk_cmd *)ctx;
	struct request *req = cmd->req;

	if (cmd->spdk_buf) {
		flux_spdk_ops_mempool_put(flux_spdk->dma_mempool, cmd->spdk_buf);
		cmd->spdk_buf = NULL;
	}

	BUG_ON(flux_spdk_ops_cpl_is_error(cpl));

	cmd->poll_ctx->qlen--;
	blk_mq_end_request(req, BLK_STS_OK);
}

static int spdk_write_copy(struct spdk_cmd *cmd, struct request *rq,
			   uint64_t lba, uint32_t lba_count)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	struct spdk_poll_ctx *ctx = cmd->poll_ctx;
	int rc;
	char *buf = NULL;

	buf = flux_spdk_ops_mempool_get(flux_spdk->dma_mempool);
	if (unlikely(!buf))
		return -ENOMEM;
	cmd->spdk_buf = buf;

	rq_for_each_segment(bvec, cmd->req, iter) {
		/*
		 * Copying from bv_page would not work in systems with MMU.
		 * However in flux memory is always mapped.
		 */
		memcpy(buf, page_address(bvec.bv_page) + bvec.bv_offset,
		       bvec.bv_len);
		buf += bvec.bv_len;
	}

	rc = flux_spdk_ops_ns_cmd_write(ctx->dev->ns_entry.ns, ctx->qpair,
				       cmd->spdk_buf, lba, lba_count,
				       spdk_write_completion_cb, cmd, 0);

	if (unlikely(rc < 0)) {
		flux_spdk_ops_mempool_put(flux_spdk->dma_mempool, cmd->spdk_buf);
		cmd->spdk_buf = NULL;
		return rc;
	}

	return rc;
}

static int spdk_write_zerocopy(struct spdk_cmd *cmd, struct request *rq,
			       uint64_t lba, uint32_t lba_count)
{
	struct spdk_poll_ctx *ctx = cmd->poll_ctx;

	return flux_spdk_ops_ns_cmd_writev(ctx->dev->ns_entry.ns, ctx->qpair,
					  lba, lba_count,
					  spdk_write_completion_cb, cmd, 0,
					  reset_sgl, next_sgl);
}

static __always_inline int spdk_write(struct spdk_cmd *cmd, struct request *rq,
				      uint64_t lba, uint32_t lba_count)
{
	if (flux_spdk->zero_copy) {
		return spdk_write_zerocopy(cmd, rq, lba, lba_count);
	} else {
		return spdk_write_copy(cmd, rq, lba, lba_count);
	}
}

static struct spdk_poll_ctx *spdk_get_poll_context(struct spdk_blkdev *dev,
						   struct blk_mq_hw_ctx *hctx)
{
	return &dev->poll_contexts[smp_processor_id()];
}

static int spdk_complete_requests(struct spdk_poll_ctx *ctx)
{
	int rc, n = 0;

	do {
		rc = flux_spdk_ops_qpair_process_completions(ctx->qpair, 0);
		if (rc > 0)
			n += rc;
	} while (rc > 0);

	return n;
}

static void spdk_process_request(struct request *rq, struct spdk_poll_ctx *ctx)
{
	struct spdk_nvme_ns *ns = ctx->dev->ns_entry.ns;
	struct spdk_cmd *cmd = blk_mq_rq_to_pdu(rq);
	size_t len = blk_rq_bytes(rq);
	uint32_t lba_count;
	uint64_t lba;
	int sector_size;

	if (!len) {
		blk_mq_end_request(rq, BLK_STS_OK);
		return;
	}

	cmd->req = rq;
	cmd->poll_ctx = ctx;

	sector_size = flux_spdk_ops_ns_get_sector_size(ns);
	lba_count = len / sector_size;
	lba = blk_rq_pos(rq) * 512 / sector_size;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		if (spdk_read(cmd, rq, lba, lba_count) < 0)
			goto fail;
		break;
	case REQ_OP_WRITE:
		if (spdk_write(cmd, rq, lba, lba_count) < 0)
			goto fail;
		break;
	default:
		pr_info("process request op %u\n", req_op(rq));
		goto fail;
		break;
	}

	return;

fail:
	ctx->qlen--;
	blk_mq_end_request(rq, BLK_STS_IOERR);
}

static blk_status_t spdk_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct spdk_blkdev *dev = hctx->driver_data;
	int status = BLK_STS_IOERR;
	struct spdk_poll_ctx *ctx = spdk_get_poll_context(dev, hctx);

	blk_mq_start_request(rq);

	switch (req_op(rq)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		ctx->qlen++;
		spdk_process_request(rq, ctx);
		status = BLK_STS_OK;
		break;
	default:
		pr_warn("process request op %u\n", req_op(rq));
		break;
	}

	spdk_complete_requests(ctx);

	if (ctx->qlen)
		wake_up_interruptible(&ctx->wait_queue);

	return status;
}

static int spdk_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			  unsigned int hctx_idx)
{
	struct spdk_blkdev *dev = data;
	hctx->driver_data = dev;
	return 0;
}

static void spdk_map_queues(struct blk_mq_tag_set *set)
{
	int cpu;
	struct blk_mq_queue_map *qmap = &set->map[HCTX_TYPE_DEFAULT];

	BUG_ON(set->nr_maps > 1);
	for_each_possible_cpu(cpu) {
		qmap->mq_map[cpu] = cpu;
	}
}

const struct blk_mq_ops spdk_mq_ops = {
	.queue_rq = spdk_queue_rq,
	.init_hctx = spdk_init_hctx,
	.map_queues = spdk_map_queues,
};

static struct block_device_operations spdk_blk_mq_bdev_ops = {
	.owner = THIS_MODULE,
};

static int spdk_poll_thread(void *arg)
{
	struct spdk_poll_ctx *ctx = arg;

	while (!kthread_should_stop()) {
		if (!ctx->qlen) {
			wait_event_interruptible(ctx->wait_queue,
						 ctx->qlen > 0 ||
							 kthread_should_stop());
		}
		spdk_complete_requests(ctx);
		io_schedule();
	}

	return 0;
}

static void spdk_free_poll_contexts(struct spdk_poll_ctx *contexts, size_t num)
{
	size_t i;
	struct spdk_poll_ctx *ctx;

	for (i = 0; i < num; i++) {
		ctx = &contexts[i];
		if (ctx->thread)
			kthread_stop(ctx->thread);
	}
	kfree(contexts);
}

static int spdk_init_poll_context_on_cpumask(struct spdk_poll_ctx *ctx,
					     struct spdk_blkdev *dev,
					     struct spdk_nvme_qpair *qpair,
					     const struct cpumask *mask)
{
	struct task_struct *t = NULL;

	ctx->dev = dev;
	ctx->qpair = qpair;
	cpumask_copy(&ctx->cpumask, mask);
	init_waitqueue_head(&ctx->wait_queue);

	t = kthread_create(spdk_poll_thread, ctx, "spdk/%d-",
			   cpumask_first(mask));
	if (IS_ERR(t))
		return PTR_ERR(t);

	kthread_bind_mask(t, cpumask_of(cpumask_first(mask)));
	wake_up_process(t);

	ctx->thread = t;
	return 0;
}

static int spdk_init_poll_context(struct spdk_blkdev *dev)
{
	int i, err;
	struct spdk_poll_ctx *poll_contexts;
	struct flux_spdk_ns_entry *entry = &dev->ns_entry;

	poll_contexts = kzalloc(
		sizeof(struct spdk_poll_ctx) * entry->qpairs_num, GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	for (i = 0; i < entry->qpairs_num; i++) {
		err = spdk_init_poll_context_on_cpumask(
			&poll_contexts[i], dev, dev->ns_entry.qpairs[i],
			cpumask_of(i));
		if (err < 0)
			goto out_free_poll_ctx;
	}

	dev->poll_contexts = poll_contexts;

	return 0;
out_free_poll_ctx:
	kfree(poll_contexts);
	return err;
}

static int spdk_add_blk_mq_bdev(struct spdk_blkdev **spdk_dev,
				struct flux_spdk_ns_entry *entry)
{
	struct spdk_blkdev *dev;
	struct gendisk *disk;
	int err, idx, bs;
	sector_t size;

	if (entry->qpairs_num > num_online_cpus() || entry->qpairs_num <= 0)
		return -EINVAL;

	err = -ENOMEM;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		goto out;

	err = idr_alloc(&spdk_index_idr, dev, 0, 0, GFP_KERNEL);
	if (err < 0)
		goto out_free_dev;
	idx = err;

	memcpy(&dev->ns_entry, entry, sizeof(struct flux_spdk_ns_entry));
	dev->dev_id = idx;
	dev->tag_set.ops = &spdk_mq_ops;
	dev->tag_set.nr_hw_queues = dev->ns_entry.qpairs_num;
	dev->tag_set.queue_depth = flux_spdk->dma_mempool_size;
	dev->tag_set.numa_node = NUMA_NO_NODE;
	dev->tag_set.cmd_size = sizeof(struct spdk_cmd);
	dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE |
			     BLK_MQ_F_NO_SCHED_BY_DEFAULT;
	dev->tag_set.driver_data = dev;

	err = blk_mq_alloc_tag_set(&dev->tag_set);
	if (err)
		goto out_free_idr;

	err = -ENOMEM;
	disk = dev->spdk_disk = blk_mq_alloc_disk(&dev->tag_set, dev);
	if (!disk)
		goto out_cleanup_tags;

	disk->major = spdk_bdev_major;
	disk->first_minor = idx;
	disk->minors = 1;
	disk->fops = &spdk_blk_mq_bdev_ops;
	disk->private_data = dev;
	sprintf(disk->disk_name, "spdk%d", idx);

	err = spdk_init_poll_context(dev);
	if (err < 0)
		goto out_cleanup_disk;

	bs = flux_spdk_ops_ns_get_sector_size(dev->ns_entry.ns);
	size = flux_spdk_ops_ns_get_size(dev->ns_entry.ns) / bs;
	set_capacity(disk, size);
	blk_queue_logical_block_size(disk->queue, bs);
	blk_queue_physical_block_size(disk->queue, bs);
	blk_queue_io_min(disk->queue, bs);
	blk_queue_max_hw_sectors(disk->queue, BLK_DEF_MAX_SECTORS);

	err = add_disk(disk);
	if (err)
		goto out_free_poll_ctx;

	*spdk_dev = dev;

	pr_info("bdev %s added with id %d, size %llu sectors\n",
		disk->disk_name, dev->dev_id,
		(unsigned long long)size);

	return dev->dev_id;
out_free_poll_ctx:
	spdk_free_poll_contexts(dev->poll_contexts, entry->qpairs_num);
out_cleanup_disk:
	put_disk(disk);
out_cleanup_tags:
	blk_mq_free_tag_set(&dev->tag_set);
out_free_idr:
	idr_remove(&spdk_index_idr, idx);
out_free_dev:
	kfree(dev);
out:
	return err;
}

static int spdk_exit_cb(int id, void *ptr, void *data)
{
	struct spdk_blkdev *dev = ptr;

	del_gendisk(dev->spdk_disk);
	put_disk(dev->spdk_disk);
	blk_mq_free_tag_set(&dev->tag_set);
	spdk_free_poll_contexts(dev->poll_contexts, dev->ns_entry.qpairs_num);
	kfree(dev);
	return 0;
}

static void spdk_remove_devices(void)
{
	idr_for_each(&spdk_index_idr, &spdk_exit_cb, NULL);
	idr_destroy(&spdk_index_idr);
}

static void spdk_dump_all_files(void)
{
	struct task_struct *p;
	unsigned int fd;
	char *path;
	static char path_buf[256];
	struct file *fd_file;

	rcu_read_lock();
	for_each_process(p) {
		fd = 0;
		do {
			fd_file = task_lookup_next_fd_rcu(p, &fd);
			if (fd_file) {
				path = d_path(&fd_file->f_path, path_buf,
					      sizeof(path_buf));
				if (IS_ERR(path)) {
					printk("fd: %d: comm: %s\n", fd,
					       p->comm);
				} else {
					printk("fd: %d: comm: %s path %s\n", fd,
					       p->comm, path);
				}
			}
		} while (fd_file);
	}
	rcu_read_unlock();
}

static long spdk_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct spdk_blkdev *dev;
	long ret = -ENOSYS;

	switch (cmd) {
	case SPDK_IOCTL_ADD:
		mutex_lock(&spdk_mutex);
		ret = spdk_add_blk_mq_bdev(&dev, (void *)arg);
		mutex_unlock(&spdk_mutex);
		break;
	case SPDK_IOCTL_COMPLETE:
		blk_mq_complete_request((void *)arg);
		break;
	case SPDK_IOCTL_DEBUG:
		spdk_dump_all_files();
		fallthrough;
	case SPDK_IOCTL_SHUTDOWN:
		spdk_exit();
		break;
	default:
		pr_err("unknown ioctl %u\n", cmd);
		break;
	}

	return ret;
}

static const struct file_operations spdk_fops = {
	.open = nonseekable_open,
	.unlocked_ioctl = spdk_ioctl,
	.compat_ioctl = spdk_ioctl,
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
};

static struct miscdevice spdk_misc = {
	.name = "spdk-control",
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &spdk_fops,
};

static int __init spdk_init(void)
{
	int err;

	err = misc_register(&spdk_misc);
	if (err < 0)
		goto out;

	err = register_blkdev(0, "spdk");
	if (err < 0) {
		err = -EIO;
		goto misc_out;
	}

	spdk_bdev_major = err;

	pr_info("module loaded minor: %d\n", spdk_misc.minor);

	return 0;

misc_out:
	misc_deregister(&spdk_misc);
out:
	return err;
}

static void spdk_exit(void)
{
	spdk_remove_devices();
	unregister_blkdev(spdk_bdev_major, "spdk");
	misc_deregister(&spdk_misc);
}

static void __exit _spdk_exit(void)
{
	spdk_exit();
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SPDK block device driver");

module_init(spdk_init);
module_exit(_spdk_exit);
