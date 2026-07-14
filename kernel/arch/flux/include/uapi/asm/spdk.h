#ifndef _ASM_UAPI_FLUX_SPDK_H
#define _ASM_UAPI_FLUX_SPDK_H

#ifdef CONFIG_FLUX_SPDK

#ifdef __KERNEL__
#include <linux/types.h>

struct spdk_nvme_ctrlr;
struct spdk_ctrlr_entry;
struct spdk_nvme_ns;
struct spdk_ns_entry;
struct spdk_nvme_qpair;
struct spdk_nvme_cpl;
struct spdk_mempool;
typedef void (*spdk_nvme_cmd_cb)(void *, const struct spdk_nvme_cpl *);
typedef void (*spdk_nvme_req_reset_sgl_cb)(void *cb_arg, uint32_t offset);
typedef int (*spdk_nvme_req_next_sge_cb)(void *cb_arg, void **address,
					 uint32_t *length);
#else
#include <spdk/env.h>
#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#endif

#define SPDK_IOCTL_ADD 0x4C80
#define SPDK_IOCTL_ADD_DIRECT 0x4C81
#define SPDK_IOCTL_COMPLETE 0x4C82
#define SPDK_IOCTL_SHUTDOWN 0x4C83
#define SPDK_IOCTL_DEBUG 0x4C84

struct flux_spdk_operations {
	int (*cpl_is_error)(const struct spdk_nvme_cpl *cpl);

	uint64_t (*ns_get_size)(struct spdk_nvme_ns *ns);
	uint32_t (*ns_get_sector_size)(struct spdk_nvme_ns *ns);

	int (*ns_cmd_write)(struct spdk_nvme_ns *ns,
			    struct spdk_nvme_qpair *qpair, void *payload,
			    uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			    uint32_t io_flags);
	int (*ns_cmd_writev)(struct spdk_nvme_ns *ns,
			     struct spdk_nvme_qpair *qpair, uint64_t lba,
			     uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg, uint32_t io_flags,
			     spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			     spdk_nvme_req_next_sge_cb next_sge_fn);
	int (*ns_cmd_read)(struct spdk_nvme_ns *ns,
			   struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			   uint32_t io_flags);
	int (*ns_cmd_readv)(struct spdk_nvme_ns *ns,
			    struct spdk_nvme_qpair *qpair, uint64_t lba,
			    uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			    void *cb_arg, uint32_t io_flags,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn);

	void *(*mempool_get)(struct spdk_mempool *mp);
	void (*mempool_put)(struct spdk_mempool *mp, void *buf);

	int32_t (*qpair_process_completions)(struct spdk_nvme_qpair *qpair,
					     uint32_t timeout);
};

struct flux_spdk_ctrlr_entry {
	struct flux_spdk_ctrlr_entry *next;
	struct spdk_nvme_ctrlr *ctrlr;
	char name[1024];
};

struct flux_spdk_ns_entry {
	struct flux_spdk_ns_entry *next;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair **qpairs;
	unsigned long qpairs_num;
};

struct flux_spdk_dma_region {
	uint64_t start, end;
};

struct flux_spdk {
	bool zero_copy;
	struct spdk_mempool *dma_mempool;
	uint64_t dma_mempool_size;
	struct flux_spdk_operations ops;
};

#endif /* CONFIG_FLUX_SPDK */

#endif /* _ASM_UAPI_FLUX_SPDK_H */