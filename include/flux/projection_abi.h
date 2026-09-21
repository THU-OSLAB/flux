/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _FLUX_PROJECTION_ABI_H
#define _FLUX_PROJECTION_ABI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Read-only host MM feature query. Successful ioctl returns a bit mask. */
#define FLUX_DEV_IO_MM_FEATURES _IO(0x1000, 0x1c)
#define FLUX_MM_FEATURE_HUGETLB_PMD_SHARE 1UL

/* Construction-only: the child Flux mmap write lock protects this snapshot. */
#define FLUX_DEV_IO_PREPARE_FORK_FAULTS _IO(0x1000, 0x1b)
struct flux_prepare_fork_faults {
	__u32 proc_key;
	__u32 flags;
	__u64 pgd;
	__u64 memory_start;
	__u64 memory_end;
};

/*
 * Version 2: bind an unpublished fork child's software root and a sorted
 * whitelist of readable private-anonymous VMAs. No leaf pages are copied or
 * pinned at registration. Any invalidation retires overlapping ranges before
 * guest pages/tables can be reused; exit unbinds the root before page release.
 */
#define FLUX_DEV_IO_BIND_FORK_PROJECTION _IO(0x1000, 0x1d)
#define FLUX_DEV_IO_FLUSH_USER_MM _IO(0x1000, 0x1e)
#define FLUX_PROJECTION_MAX_RANGES 4096U
struct flux_projection_range {
	__u64 start;
	__u64 end;
};

struct flux_bind_fork_projection {
	__u32 proc_key;
	__u32 nr_ranges;
	__u64 pgd;
	__u64 memory_start;
	__u64 memory_end;
	__u64 ranges;
};

_Static_assert(sizeof(struct flux_projection_range) == 16,
	       "flux_projection_range size");
_Static_assert(sizeof(struct flux_bind_fork_projection) == 40,
	       "flux_bind_fork_projection size");

/* Version 1 accepts only the existing four-level, base-page Flux PTE format. */
#define FLUX_PROJECTION_PT_PRESENT 0x001ULL
#define FLUX_PROJECTION_PT_HUGE 0x008ULL
#define FLUX_PROJECTION_PT_NONE 0x010ULL
#define FLUX_PROJECTION_PT_USER 0x040ULL
#define FLUX_PROJECTION_PT_XOL 0x004ULL
#define FLUX_PROJECTION_PT_EXEC 0x800ULL

#endif
