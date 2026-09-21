/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _FLUX_ELASTIC_ABI_H
#define _FLUX_ELASTIC_ABI_H

/* Shared protocol between the userspace Flux kernel, host module, and host tools. */
#include <linux/ioctl.h>
#include <linux/types.h>

#define FLUX_ELASTIC_EXTENT_SHIFT 21
#define FLUX_ELASTIC_EXTENT_SIZE (1UL << FLUX_ELASTIC_EXTENT_SHIFT)
#define FLUX_ELASTIC_MAX_EXTENTS 256

/* CREATE is issued on /dev/flux_mm and returns a CLOEXEC domain fd. */
#define FLUX_DEV_IO_ELASTIC_CREATE _IO(0x1000, 0x1a)
struct flux_elastic_create {
	__u32 nr_extents;
	__u32 flags; /* Must be zero. All extents initially have no backing. */
};

#define FLUX_ELASTIC_UNBACKED 0
#define FLUX_ELASTIC_AVAILABLE 1

/* Generation starts at 1 and changes on each successful state transition. */
struct flux_elastic_extent {
	__u32 index;
	__u32 reserved;
	__u64 generation; /* Expected generation for COMMIT/DECOMMIT. */
	__u32 state;
	__u32 resident_pages;
};

struct flux_elastic_info {
	__u32 nr_extents;
	__u32 extent_shift;
	__u64 resident_pages; /* Unique backing held by this domain. */
	__u64 commits;
	__u64 decommits;
	__u64 busy;
	__u64 faults;
	__u64 denied_faults;
};

#define FLUX_ELASTIC_QUERY _IOWR('E', 0x1, struct flux_elastic_extent)
#define FLUX_ELASTIC_COMMIT _IOWR('E', 0x2, struct flux_elastic_extent)
#define FLUX_ELASTIC_DECOMMIT _IOWR('E', 0x3, struct flux_elastic_extent)
#define FLUX_ELASTIC_INFO _IOR('E', 0x4, struct flux_elastic_info)

#endif
