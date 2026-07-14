#ifndef _FLUX_LIB_IO_SPDK_H
#define _FLUX_LIB_IO_SPDK_H

#ifdef CONFIG_FLUX_SPDK

#include <stddef.h>

#include <kernel/asm/spdk.h>

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/nvme.h>
#include <spdk/version.h>

struct flux_spdk_context {
	struct flux_spdk_ns_entry *ns_head;
	struct flux_spdk_ctrlr_entry *ctrlr_head;
	void *spdk_nvme_driver;
	char *key;
	unsigned long key_len;
	int attach_error;
	int skip_unmount;
};

struct flux_spdk_dma_memory {
	struct spdk_mempool *data_pool;
	size_t data_pool_size;
};

struct flux_spdk_dev {
	int ioctl_fd;
	struct flux_spdk_context ctx;
	struct flux_spdk_dma_memory dma;
};

/* must be called before kernel */
int flux_spdk_init(void);
void flux_spdk_fini(void);
int flux_spdk_register_dev(struct flux_spdk_dev *dev);

#define SPDK_DATA_POOL_ELEM_SIZE (2UL << 20)
#define SPDK_DATA_POOL_CACHE_SIZE 256

#endif /* CONFIG_FLUX_SPDK */

#endif /* _FLUX_LIB_IO_SPDK_H */
