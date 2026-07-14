#ifndef _SPDK_BLK_DEV_H
#define _SPDK_BLK_DEV_H

#include <linux/blk-mq.h>
#include <linux/interrupt.h>
#include <uapi/asm/spdk.h>

struct spdk_blkdev {
	struct flux_spdk_ns_entry ns_entry;
	int dev_id;
	struct blk_mq_tag_set tag_set;
	struct gendisk *spdk_disk;
	struct llist_head spdk_queue;
	struct spdk_poll_ctx *poll_contexts;
};

struct spdk_poll_ctx {
	struct cpumask cpumask;
	struct task_struct *thread;
	struct spdk_blkdev *dev;
	struct spdk_nvme_qpair *qpair;
	size_t qlen;
	wait_queue_head_t wait_queue;
} __aligned(L1_CACHE_BYTES);

struct spdk_cmd {
	void *spdk_buf;
	struct spdk_poll_ctx *poll_ctx;
	struct request *req;
	struct req_iterator iter;
	unsigned long long ts;
	uint32_t iov_offset;
};

#endif
