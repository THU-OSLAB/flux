/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_DMA_MAPPING_H
#define _ASM_FLUX_DMA_MAPPING_H

extern const struct dma_map_ops flux_dma_ops;

static inline const struct dma_map_ops *get_arch_dma_ops(void)
{
	return &flux_dma_ops;
}

#endif
