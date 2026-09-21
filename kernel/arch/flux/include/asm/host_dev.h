#ifndef _ASM_FLUX_HOST_DEV_H
#define _ASM_FLUX_HOST_DEV_H

#include <flux/host_abi.h>
#include <asm/syscalls.h>
#include <asm/unistd.h>

extern int flux_host_dev_fd;

extern void flux_host_dev_init(void);
extern void flux_host_dev_exit(void);

#define flux_host_dev_call_mm(cmd, arg) \
	host_syscall(__NR_ioctl, flux_host_dev_fd, cmd, arg)

static inline long
flux_host_dev_reserve_alias_range(unsigned long addr, unsigned long len,
				  bool shared)
{
	struct flux_alias_reserve_args a = {
		.user_va = addr,
		.len = len,
		.flags = shared ? FLUX_ALIAS_F_SHARED : 0,
		.pad = 0,
	};
	/* arch_prepare_mmap consumes a status: zero succeeds, errno fails. */
	return flux_host_dev_call_mm(FLUX_DEV_IO_RESERVE_ALIAS_RANGE,
				    (unsigned long)&a);
}

static inline int flux_host_dev_copy_mm(bool for_fork)
{
	int proc_key, err;

	err = flux_host_dev_call_mm(for_fork ? FLUX_DEV_IO_COPY_MM :
				   FLUX_DEV_IO_COPY_EXEC_MM, &proc_key);
	if (err < 0)
		return err;

	return proc_key;
}

static inline int flux_host_dev_release_mm(int proc_key)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_RELEASE_MM, proc_key);
}

static inline int flux_host_dev_switch_mm(int proc_key_to, int proc_key_from)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_SWITCH_MM,
				     ((u64)proc_key_to << 32) | proc_key_from);
}

static inline int
__flux_host_dev_alias_pages(unsigned long user_va, unsigned long src_va,
			    unsigned long nr, int prot, bool shared, bool nowait,
			    unsigned long expected_pte_addr, u64 expected_pte,
			    u64 expected_pte_mask, bool repeat_source)
{
	struct flux_alias_args a = {
		.user_va = user_va,
		.src_va = src_va,
		.nr = nr,
		.prot = (u32)prot,
#ifdef CONFIG_FLUX_MPK
		.pkey = FLUX_MPK_APP_PKEY,
#else
		.pkey = 0,
#endif
		.flags = (shared ? FLUX_ALIAS_F_SHARED : 0) |
			 (nowait ? FLUX_ALIAS_F_NOWAIT : 0) |
			 (expected_pte_addr ? FLUX_ALIAS_F_CHECK_PTE : 0) |
			 (repeat_source ? FLUX_ALIAS_F_REPEAT_SOURCE : 0),
		.pad = 0,
		.expected_pte_addr = expected_pte_addr,
		.expected_pte = expected_pte,
		.expected_pte_mask = expected_pte_mask,
	};

	return flux_host_dev_call_mm(FLUX_DEV_IO_ALIAS_PAGES,
				     (unsigned long)&a);
}

static inline int flux_host_dev_alias_pages(unsigned long user_va,
					    unsigned long src_va,
					    unsigned long nr, int prot,
					    bool shared, bool nowait)
{
	return __flux_host_dev_alias_pages(user_va, src_va, nr, prot, shared,
					   nowait, 0, 0, 0, false);
}

/* Normal-context repeated-source run; no NOWAIT fallback is implied. */
static inline int
flux_host_dev_alias_repeated_pages(unsigned long user_va, unsigned long src_va,
				  unsigned long nr, int prot, bool shared)
{
	return __flux_host_dev_alias_pages(user_va, src_va, nr, prot, shared,
					   false, 0, 0, 0, true);
}

static inline int
flux_host_dev_alias_kernel_page_checked(unsigned long user_va,
					unsigned long src_va, int prot,
					bool nowait,
					unsigned long expected_pte_addr,
					u64 expected_pte, u64 expected_pte_mask)
{
	return __flux_host_dev_alias_pages(user_va, src_va, 1, prot, false,
					   nowait, expected_pte_addr,
					   expected_pte, expected_pte_mask, false);
}

static inline int flux_host_dev_rekey_aliases(unsigned long user_va,
					       unsigned long nr, int pkey)
{
	struct flux_rekey_alias_args a = {
		.user_va = user_va,
		.nr = nr,
		.pkey = (u32)pkey,
	};

	return flux_host_dev_call_mm(FLUX_DEV_IO_REKEY_ALIASES,
				     (unsigned long)&a);
}

static inline int flux_host_dev_unalias_user_mm(int proc_key)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_UNALIAS_USER_MM, proc_key);
}

static inline int flux_host_dev_flush_user_mm(int proc_key)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_FLUSH_USER_MM, proc_key);
}

static inline int flux_host_dev_unalias_pages(int proc_key,
					       unsigned long user_va,
					       unsigned long nr)
{
	struct flux_unalias_args a = {
		.user_va = user_va,
		.nr = nr,
		.proc_key = (u32)proc_key,
	};

	return flux_host_dev_call_mm(FLUX_DEV_IO_UNALIAS_PAGES,
				     (unsigned long)&a);
}

static inline int
flux_host_dev_unalias_kernel_pages(unsigned long user_va, unsigned long nr)
{
	struct flux_kernel_unalias_args a = {
		.user_va = user_va,
		.nr = nr,
	};

	return flux_host_dev_call_mm(FLUX_DEV_IO_UNALIAS_KERNEL_PAGES,
				     (unsigned long)&a);
}

static inline int flux_host_dev_fork_alias_begin(int proc_key)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_FORK_ALIAS_BEGIN, proc_key);
}

static inline int flux_host_dev_fork_alias_end(int proc_key)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_FORK_ALIAS_END, proc_key);
}


static inline int flux_host_dev_enable_mpk(void)
{
	return flux_host_dev_call_mm(FLUX_DEV_IO_ENABLE_MPK, 0);
}

static inline int
flux_host_dev_query_app_range(unsigned long start, unsigned long len)
{
	struct flux_mpk_range range = {
		.start = start,
		.len = len,
	};

	return flux_host_dev_call_mm(FLUX_DEV_IO_VALIDATE_APP_RANGE, &range);
}

static inline int
flux_host_dev_validate_app_range(unsigned long start, unsigned long len)
{
	int ret = flux_host_dev_query_app_range(start, len);

	/* Preserve the original validate-only contract for existing callers. */
	return ret < 0 ? ret : 0;
}

static inline int
flux_host_dev_get_base_maps(struct flux_mpk_base_map *maps,
			    unsigned int capacity, unsigned int *nr)
{
	struct flux_mpk_base_maps req = {
		.maps = (unsigned long)maps,
		.capacity = capacity,
	};
	int ret;

	ret = flux_host_dev_call_mm(FLUX_DEV_IO_GET_BASE_MAPS, &req);
	*nr = req.nr;
	return ret;
}

static inline int
flux_host_dev_get_app_maps(int proc_key, struct flux_mpk_base_map *maps,
			   unsigned int capacity, unsigned int *nr)
{
	struct flux_mm_app_maps req = {
		.maps = (unsigned long)maps,
		.capacity = capacity,
		.proc_key = proc_key,
	};
	int ret;

	ret = flux_host_dev_call_mm(FLUX_DEV_IO_GET_APP_MAPS, &req);
	*nr = req.nr;
	return ret;
}

static inline int
flux_host_dev_get_app_smaps(int proc_key, struct flux_mm_smaps_stats *stats)
{
	stats->proc_key = proc_key;
	return flux_host_dev_call_mm(FLUX_DEV_IO_GET_APP_SMAPS, stats);
}

#endif /* _ASM_FLUX_HOST_DEV_H */
