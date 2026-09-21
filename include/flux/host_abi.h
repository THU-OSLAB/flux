/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _FLUX_HOST_ABI_H
#define _FLUX_HOST_ABI_H

/* Shared device contract for the host module, Flux runtime, and Flux kernel.
 * Keep ioctl numbers and wire layouts here; callers own their local wrappers.
 * Flux currently exposes this protocol only to native x86-64 processes.
 */
#include <linux/ioctl.h>
#include <linux/types.h>
#include <flux/signal_abi.h>
#include <flux/elastic_abi.h>
#include <flux/projection_abi.h>

/* Character devices are allocated dynamically at module init. */

enum {
	FLUX_DEV = 0,
	FLUX_UINTR_DEV,
	FLUX_MM_DEV,
	FLUX_LAST_DEV,
	FLUX_NR_DEVS = FLUX_LAST_DEV,
};

#define FLUX_DEV_IO_BASE 0x1000
#define FLUX_DEV_IO_UINTR_SETUP _IO(FLUX_DEV_IO_BASE, 0x1)
#define FLUX_DEV_IO_START_MAP_SHARED _IO(FLUX_DEV_IO_BASE, 0x2)
#define FLUX_DEV_IO_END_MAP_SHARED _IO(FLUX_DEV_IO_BASE, 0x3)
#define FLUX_DEV_IO_EXECVE _IO(FLUX_DEV_IO_BASE, 0x4)
#define FLUX_DEV_IO_DUMP_VMAS _IO(FLUX_DEV_IO_BASE, 0x5)
#define FLUX_DEV_IO_COPY_MM _IO(FLUX_DEV_IO_BASE, 0x6)
#define FLUX_DEV_IO_RELEASE_MM _IO(FLUX_DEV_IO_BASE, 0x7)
#define FLUX_DEV_IO_SWITCH_MM _IO(FLUX_DEV_IO_BASE, 0x8)
#define FLUX_DEV_IO_RESERVED_09 _IO(FLUX_DEV_IO_BASE, 0x9)
#define FLUX_DEV_IO_ENABLE_MPK _IO(FLUX_DEV_IO_BASE, 0xa)
#define FLUX_DEV_IO_VALIDATE_APP_RANGE _IO(FLUX_DEV_IO_BASE, 0xb)
#define FLUX_MPK_APP_RANGE_ALIAS 1
#define FLUX_MPK_APP_RANGE_SHADOW 2
#define FLUX_DEV_IO_GET_BASE_MAPS _IO(FLUX_DEV_IO_BASE, 0xc)
#define FLUX_DEV_IO_GET_APP_MAPS _IO(FLUX_DEV_IO_BASE, 0xd)
#define FLUX_DEV_IO_GET_APP_SMAPS _IO(FLUX_DEV_IO_BASE, 0xe)
#define FLUX_DEV_IO_ALIAS_PAGES _IO(FLUX_DEV_IO_BASE, 0xf)
#define FLUX_DEV_IO_UNALIAS_PAGES _IO(FLUX_DEV_IO_BASE, 0x10)
#define FLUX_DEV_IO_REPROTECT_ALIASES _IO(FLUX_DEV_IO_BASE, 0x11)
#define FLUX_DEV_IO_FORK_ALIAS_BEGIN _IO(FLUX_DEV_IO_BASE, 0x12)
#define FLUX_DEV_IO_FORK_ALIAS_END _IO(FLUX_DEV_IO_BASE, 0x13)
#define FLUX_DEV_IO_REKEY_ALIASES _IO(FLUX_DEV_IO_BASE, 0x14)
#define FLUX_DEV_IO_UNALIAS_KERNEL_PAGES _IO(FLUX_DEV_IO_BASE, 0x15)
#define FLUX_DEV_IO_RESERVE_ALIAS_RANGE _IO(FLUX_DEV_IO_BASE, 0x16)
/* COPY_MM creates a fork slot; exec construction has a separate intent. */
#define FLUX_DEV_IO_COPY_EXEC_MM _IO(FLUX_DEV_IO_BASE, 0x17)
#define FLUX_DEV_IO_TAKE_FRAME_UINTR _IO(FLUX_DEV_IO_BASE, 0x18)
#define FLUX_DEV_IO_UNALIAS_USER_MM _IO(FLUX_DEV_IO_BASE, 0x19)

struct flux_mpk_range {
	unsigned long start;
	unsigned long len;
};

/*
 * skas page aliasing: install, at each application VA [user_va + i*PAGE), a
 * special (VM_PFNMAP) pte pointing at the same host pfn that currently backs
 * src_va + i*PAGE (the Flux buddy page via its kernel direct-map alias). This
 * gives application memory ONE physical backing shared with the Flux kernel
 * view, so mm features (ksm/numa/hugetlb) and host CPU access stay consistent.
 */
/* Fixed-address inaccessible alias VMA; ioctl returns 0/errno, not a VA. */
struct flux_alias_reserve_args {
	__u64 user_va;
	__u64 len;
	__u32 flags; /* FLUX_ALIAS_F_SHARED only */
	__u32 pad;
};

struct flux_alias_args {
	__u64 user_va;
	__u64 src_va;
	__u64 nr;
	__u32 prot; /* PROT_READ/WRITE/EXEC */
	__u32 pkey;
	__u32 flags;
	__u32 pad;
	__u64 expected_pte_addr;
	__u64 expected_pte;
	__u64 expected_pte_mask;
};

#define FLUX_ALIAS_F_SHARED 0x1
#define FLUX_ALIAS_F_NOWAIT 0x2
#define FLUX_ALIAS_F_CHECK_PTE 0x4
/* All target pages share src_va, resolved once per normal request. */
#define FLUX_ALIAS_F_REPEAT_SOURCE 0x8

struct flux_rekey_alias_args {
	__u64 user_va;
	__u64 nr;
	__u32 pad;
	__u32 pkey;
};

struct flux_kernel_unalias_args {
	__u64 user_va;
	__u64 nr;
};

/* Drop stale SKAS host aliases belonging to a non-current Flux mm. */
struct flux_unalias_args {
	__u64 user_va;
	__u64 nr;
	__u32 proc_key;
	__u32 pad;
};

struct flux_mpk_base_map {
	unsigned long start;
	unsigned long end;
};

struct flux_mpk_base_maps {
	unsigned long maps;
	unsigned int capacity;
	unsigned int nr;
};

struct flux_mm_app_maps {
	unsigned long maps;
	unsigned int capacity;
	unsigned int nr;
	int proc_key;
};

struct flux_mm_smaps_stats {
	__u64 start;
	__u64 end;
	__u64 size;
	__u64 resident;
	__u64 pss;
	__u64 pss_dirty;
	__u64 pss_anon;
	__u64 pss_file;
	__u64 pss_shmem;
	__u64 shared_clean;
	__u64 shared_dirty;
	__u64 private_clean;
	__u64 private_dirty;
	__u64 referenced;
	__u64 anonymous;
	__u64 ksm;
	__u64 lazyfree;
	__u64 anonymous_thp;
	__u64 shmem_thp;
	__u64 file_thp;
	__u64 shared_hugetlb;
	__u64 private_hugetlb;
	__u64 swap;
	__u64 swap_pss;
	__u64 locked;
	__u32 nr_vmas;
	__s32 proc_key;
};

#define FLUX_DEV_NAME "flux"
#define FLUX_UINTR_DEV_NAME FLUX_DEV_NAME "_uintr"
#define FLUX_MM_DEV_NAME FLUX_DEV_NAME "_mm"
#define FLUX_DEV_PATH "/dev/" FLUX_DEV_NAME
#define FLUX_UINTR_DEV_PATH "/dev/" FLUX_UINTR_DEV_NAME
#define FLUX_MM_DEV_PATH "/dev/" FLUX_MM_DEV_NAME

struct uintr_upid {
	union {
		struct {
			__u8 status; /* bit 0: ON, bit 1: SN, bit 2-7: reserved */
			__u8 reserved1; /* Reserved */
			__u8 nv; /* Notification vector */
			__u8 reserved2; /* Reserved */
			__u32 ndst; /* Notification destination */
		} nc __attribute__((packed)); /* Notification control */
		unsigned long word_val;
	};
	__u64 puir; /* Posted user interrupt requests */
} __attribute__((aligned(64)));

struct flux_shm_percpu {
	struct uintr_upid upid;
};

struct flux_shm {
	int nr_cpus;
	int used_cpus;
	struct flux_shm_percpu pcpu[];
};
struct flux_execve_args {
	char *filename;
	char **argv;
	char **envp;
};

_Static_assert(sizeof(struct uintr_upid) == 64, "uintr_upid size");
_Static_assert(__builtin_offsetof(struct flux_shm, pcpu) == 64,
	       "flux_shm pcpu offset");
_Static_assert(sizeof(struct flux_alias_args) == 64, "flux_alias_args size");
_Static_assert(sizeof(struct flux_mm_smaps_stats) == 208,
	       "flux_mm_smaps_stats size");

#endif /* _FLUX_HOST_ABI_H */
