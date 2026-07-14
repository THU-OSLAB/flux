/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_CACHE_H
#define _ASM_FLUX_CACHE_H

#include <linux/linkage.h>

/* L1 cache line size */
#define L1_CACHE_SHIFT	6
#define L1_CACHE_BYTES	(1 << L1_CACHE_SHIFT)

#define __read_mostly __section(".data..read_mostly")

#endif /* _ASM_FLUX_CACHE_H */
