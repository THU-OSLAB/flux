#define FLUX_FMT "iokd-dma: "

#include <errno.h>
#include <string.h>

#include <rte_dev.h>
#include <rte_errno.h>
#include <rte_memory.h>
#include <utils/base.h>
#include <utils/log.h>

#include "dpdk_errno.h"
#include "iokd.h"

int flux_iok_dma_map(struct flux_iok_ctrl *ctrl, void *buf, size_t len,
		     size_t pgsize, uintptr_t *physaddrs)
{
	int ret;
	rte_iova_t iova;
	size_t i;
	size_t nr_pages;
	void *pg;

	ret = rte_extmem_register(buf, len, NULL, 0, pgsize);
	if (ret < 0)
		return -rte_errno;

	if (!ctrl->iova_mode_pa) {
		ret = rte_dev_dma_map(ctrl->dev, buf, (rte_iova_t)buf, len);
		if (ret < 0) {
			if (rte_errno == EOPNOTSUPP)
				return 0;
			rte_extmem_unregister(buf, len);
			return -rte_errno;
		}
		return 0;
	}

	nr_pages = div_up(len, pgsize);
	for (i = 0; i < nr_pages; i++) {
		pg = buf + i * pgsize;
		iova = physaddrs ? physaddrs[i] : rte_mem_virt2iova(pg);
		ret = rte_dev_dma_map(ctrl->dev, pg, iova, pgsize);
		if (ret < 0) {
			size_t j;

			if (rte_errno == EOPNOTSUPP)
				break;
			for (j = 0; j < i; j++) {
				pg = buf + j * pgsize;
				iova = physaddrs ? physaddrs[j] :
						   rte_mem_virt2iova(pg);
				rte_dev_dma_unmap(ctrl->dev, pg, iova, pgsize);
			}
			rte_extmem_unregister(buf, len);
			return -rte_errno;
		}
	}

	return 0;
}

void flux_iok_dma_unmap(struct flux_iok_ctrl *ctrl, void *buf, size_t len,
			size_t pgsize, uintptr_t *physaddrs)
{
	int ret;
	rte_iova_t iova;
	size_t i;
	size_t nr_pages;
	void *pg;

	if (!ctrl->iova_mode_pa) {
		ret = rte_dev_dma_unmap(ctrl->dev, buf, (rte_iova_t)buf, len);
		if (ret < 0 && rte_errno != EOPNOTSUPP)
			FLUX_LOG(FLUX_LOG_WARN, "dma unmap failed: %s\n",
				 strerror(rte_errno));
		rte_extmem_unregister(buf, len);
		return;
	}

	nr_pages = div_up(len, pgsize);
	for (i = 0; i < nr_pages; i++) {
		pg = buf + i * pgsize;
		iova = physaddrs ? physaddrs[i] : rte_mem_virt2iova(pg);
		ret = rte_dev_dma_unmap(ctrl->dev, pg, iova, pgsize);
		if (ret < 0 && rte_errno != EOPNOTSUPP)
			FLUX_LOG(FLUX_LOG_WARN, "dma unmap failed: %s\n",
				 strerror(rte_errno));
	}

	rte_extmem_unregister(buf, len);
}
