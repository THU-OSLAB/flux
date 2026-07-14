// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "sys: " fmt

#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/numa_balancing.h>
#include <linux/entry-common.h>
#include <linux/linkage.h>
#include <linux/mutex.h>
#include <linux/syscalls.h>
#include <linux/nospec.h>
#include <linux/mman.h>
#include <linux/shm.h>
#include <linux/user_events.h>
#include <linux/tsacct_kern.h>
#include <asm-generic/ioctls.h>
#include <asm/cache.h>
#include <asm/syscalls.h>
#include <asm/host_dev.h>
#include <asm/host_fs.h>
#include <asm/x86/syscall.h>
#include <asm/x86/processor.h>
#include <uapi/asm/flux_ops.h>

typedef long (*syscall_ptr_t)(long a1, ...);

#ifdef CONFIG_FLUX_MPK
static DEFINE_MUTEX(flux_app_mm_lock);

static __always_inline bool flux_wx_prot(unsigned long prot)
{
	return (prot & (PROT_WRITE | PROT_EXEC)) ==
	       (PROT_WRITE | PROT_EXEC);
}

#endif

#define __SYSCALL(nr, sym) [nr] = sym,
asmlinkage const void *syscall_table[] __read_mostly = {
#include <asm/syscalls_64.h>
};
#undef __SYSCALL

#define __SYSCALL(nr, sym) [nr] = #sym,
asmlinkage const char *syscall_names[] __read_mostly = {
#include <asm/syscalls_64.h>
};
#undef __SYSCALL

#ifdef CONFIG_X86_DEBUG_SYSCALL
#define dbg_sys(nr, a1, a2, a3, a4, a5, a6, ret)                               \
	pr_info("[%3ld][%14s](%llx,%llx,%llx,%llx,%llx,%llx)=%ld\n", (long)nr, \
		syscall_names[nr] + 4, (u64)(a1), (u64)(a2), (u64)(a3),        \
		(u64)(a4), (u64)(a5), (u64)(a6), (long)(ret));
#else
#define dbg_sys(nr, a1, a2, a3, a4, a5, a6, ret) \
	do {                                     \
	} while (0)
#endif

static __always_inline bool do_syscall_inst(struct pt_regs *regs, int nr)
{
	/*
	 * Convert negative numbers to very high and thus out of range
	 * numbers for comparisons.
	 */
	unsigned int unr = nr;
	syscall_ptr_t sys_fn;

	if (likely(unr < NR_syscalls)) {
		unr = array_index_nospec(unr, NR_syscalls);
		sys_fn = (syscall_ptr_t)syscall_table[unr];
		regs->ax = sys_fn(regs->di, regs->si, regs->dx, regs->r10,
				  regs->r8, regs->r9);
		return true;
	}
	return false;
}

static __always_inline bool do_syscall_func(struct pt_regs *regs, int nr)
{
	/*
	 * Convert negative numbers to very high and thus out of range
	 * numbers for comparisons.
	 */
	unsigned int unr = nr;
	syscall_ptr_t sys_fn;

	if (likely(unr < NR_syscalls)) {
		unr = array_index_nospec(unr, NR_syscalls);
		sys_fn = (syscall_ptr_t)syscall_table[unr];
		regs->ax = sys_fn(regs->si, regs->dx, regs->cx, regs->r8,
				  regs->r9, *(u64 *)regs->sp);
		return true;
	}
	return false;
}

static inline void syscall_prepare_regs(struct pt_regs *regs, long nr)
{
	/* save regs pointer */
	current->thread.regs = regs;
	/* syscall must come from user mode */
	regs->uif = regs->umode = 1;
	/* save original rax */
	regs->orig_ax = regs->ax = nr;
	/* return address has been pushed on the stack */
	regs->ip = *(u64 *)regs->sp;
	regs->sp += sizeof(u64);
}

/**
 * syscall_entry_fast - very fast path for syscalls.
 */
asmlinkage long syscall_entry_fast(long nr, long a1, long a2, long a3, long a4,
				   long a5, long a6)
{
	unsigned long unr = nr;
	syscall_ptr_t sys_fn;
	long ret = -ENOSYS;
	unsigned long ti_work;
	struct pt_regs *regs = task_sys_regs(current);

	syscall_prepare_regs(regs, nr);
	arch_enter_from_user_mode(NULL);
	instrumentation_begin();

	if (likely(unr < NR_syscalls)) {
		sys_fn = (syscall_ptr_t)syscall_table[unr];
		ret = sys_fn(a1, a2, a3, a4, a5, a6);
	}

	dbg_sys(unr, a1, a2, a3, a4, a5, a6, ret);

	instrumentation_end();
	ti_work = read_thread_flags();
	if (unlikely(ti_work & (_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL))) {
		regs->ax = ret; /* update return value */
		syscall_fast_do_signal_or_restart(regs);
	}

	arch_exit_to_user_mode();
	return ret;
}

/**
 * syscall_entry_slow - slow path for syscalls with arguments 
 * in the same registers as the `syscall` instruction.
 */
asmlinkage void syscall_entry_slow(struct pt_regs *regs, int nr)
{
	syscall_prepare_regs(regs, nr);
	nr = syscall_enter_from_user_mode(regs, nr);
	instrumentation_begin();

	if (!do_syscall_inst(regs, nr) && nr != -1)
		regs->ax = sys_ni_syscall();

	dbg_sys(nr, regs->di, regs->si, regs->dx, regs->r10, regs->r8, regs->r9,
		regs->ax);

	instrumentation_end();
	syscall_exit_to_user_mode(regs);
	syscall_ret_to_user(regs);
}

/**
 * syscall_entry_env - slow path for syscalls called from Flux
 * Env. 
 */
asmlinkage void syscall_entry_env(struct pt_regs *regs, int nr)
{
	struct pt_regs *old_regs = current->thread.regs;

	syscall_prepare_regs(regs, nr);
	nr = syscall_enter_from_user_mode(regs, nr);
	instrumentation_begin();

	if (!do_syscall_func(regs, nr) && nr != -1)
		regs->ax = sys_ni_syscall();

	dbg_sys(nr, regs->si, regs->dx, regs->cx, regs->r8, regs->r9,
		*(u64 *)((void *)(regs + 1)) + 8, regs->ax);

	instrumentation_end();

	current->thread.regs = old_regs;

	syscall_exit_to_user_mode(regs);
	syscall_ret_to_env(regs);
}

int __init syscall_init(void)
{
	pr_info("syscall table start @ 0x%llx\n", (u64)syscall_table);

	return 0;
}
late_initcall(syscall_init);

SYSCALL_DEFINE6(host_mmap, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags, unsigned long, fd,
		unsigned long, offset)
{
	long host_fd = (long)fd;
	long ret;

#ifdef CONFIG_FLUX_MPK
	if (flux_wx_prot(prot))
		return -EPERM;
#endif
	if (host_fd >= 0 && !(flags & MAP_ANONYMOUS)) {
		host_fd = flux_hostfs_get_host_fd_from_fd(host_fd);
		if (host_fd < 0)
			return host_fd;
	}

#ifdef CONFIG_FLUX_MPK
	mutex_lock(&flux_app_mm_lock);
	if (flags & MAP_FIXED) {
		ret = flux_host_dev_validate_app_range(addr, len);
		if (ret < 0)
			goto out_mmap_unlock;
	}
#endif
	ret = host_syscall(__NR_mmap, addr, len, prot, flags, host_fd, offset);
#ifdef CONFIG_FLUX_MPK
	if (!IS_ERR_VALUE(ret)) {
		long err = host_syscall(__NR_pkey_mprotect, ret, len, prot,
					FLUX_MPK_APP_PKEY, 0, 0);

		if (err < 0) {
			host_syscall(__NR_munmap, ret, len, 0, 0, 0, 0);
			ret = err;
		}
	}
out_mmap_unlock:
	mutex_unlock(&flux_app_mm_lock);
#endif
	return ret;
}

#ifdef CONFIG_FLUX_MPK
static long flux_host_app_range_syscall(unsigned int nr, unsigned long start,
					unsigned long len, unsigned long a3,
					unsigned long a4, unsigned long a5,
					unsigned long a6)
{
	long ret;

	mutex_lock(&flux_app_mm_lock);
	ret = flux_host_dev_validate_app_range(start, len);
	if (!ret)
		ret = host_syscall(nr, start, len, a3, a4, a5, a6);
	mutex_unlock(&flux_app_mm_lock);
	return ret;
}
#endif

SYSCALL_DEFINE3(host_mprotect, unsigned long, start, unsigned long, len,
		unsigned long, prot)
{
#ifdef CONFIG_FLUX_MPK
	long ret;

	if (flux_wx_prot(prot))
		return -EPERM;

	mutex_lock(&flux_app_mm_lock);
	ret = flux_host_dev_validate_app_range(start, len);
	if (!ret)
		ret = host_syscall(__NR_pkey_mprotect, start, len, prot,
				   FLUX_MPK_APP_PKEY, 0, 0);
	mutex_unlock(&flux_app_mm_lock);
	return ret;
#else
	return host_syscall(__NR_mprotect, start, len, prot);
#endif
}

SYSCALL_DEFINE2(host_munmap, unsigned long, start, unsigned long, len)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_munmap, start, len, 0, 0, 0,
					  0);
#else
	return host_syscall(__NR_munmap, start, len);
#endif
}

SYSCALL_DEFINE1(host_brk, unsigned long, brk)
{
#ifdef CONFIG_FLUX_MPK
	unsigned long old_brk = host_syscall(__NR_brk, 0);
	long ret = host_syscall(__NR_brk, brk);
	unsigned long old_end = PAGE_ALIGN(old_brk);
	unsigned long new_end = PAGE_ALIGN(ret);

	if (new_end > old_end)
		BUG_ON(host_syscall(__NR_pkey_mprotect, old_end,
				   new_end - old_end, PROT_READ | PROT_WRITE,
				   FLUX_MPK_APP_PKEY, 0, 0) < 0);
	return ret;
#else
	return host_syscall(__NR_brk, brk);
#endif
}

SYSCALL_DEFINE5(host_mremap, unsigned long, old_addr, unsigned long, old_len,
		unsigned long, new_len, unsigned long, flags, unsigned long,
		new_addr)
{
#ifdef CONFIG_FLUX_MPK
	long ret;

	mutex_lock(&flux_app_mm_lock);
	ret = flux_host_dev_validate_app_range(old_addr, old_len);
	if (!ret && (flags & MREMAP_FIXED))
		ret = flux_host_dev_validate_app_range(new_addr, new_len);
	if (!ret)
		ret = host_syscall(__NR_mremap, old_addr, old_len, new_len,
				   flags, new_addr, 0);
	mutex_unlock(&flux_app_mm_lock);
	return ret;
#else
	return host_syscall(__NR_mremap, old_addr, old_len, new_len, flags,
				    new_addr);
#endif
}

SYSCALL_DEFINE3(host_msync, unsigned long, start, unsigned long, len, int,
		flags)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_msync, start, len, flags, 0, 0,
					  0);
#else
	return host_syscall(__NR_msync, start, len, flags);
#endif
}

SYSCALL_DEFINE3(host_mincore, unsigned long, start, unsigned long, len,
		unsigned char __user *, vec)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_mincore, start, len,
					  (unsigned long)vec, 0, 0, 0);
#else
	return host_syscall(__NR_mincore, start, len, vec);
#endif
}

SYSCALL_DEFINE3(host_madvise, unsigned long, start, unsigned long, len, int,
		behavior)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_madvise, start, len, behavior,
					  0, 0, 0);
#else
	return host_syscall(__NR_madvise, start, len, behavior);
#endif
}

SYSCALL_DEFINE3(host_shmat, int, shmid, char __user *, shmaddr, int, shmflg)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_shmat, shmid, shmaddr, shmflg);
#endif
}

SYSCALL_DEFINE3(host_shmget, int, key, size_t, size, int, shmflg)
{
	return host_syscall(__NR_shmget, key, size, shmflg);
}

SYSCALL_DEFINE3(host_shmctl, int, shmid, int, cmd, unsigned long, buf)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_shmctl, shmid, cmd, buf);
#endif
}

SYSCALL_DEFINE1(host_shmdt, char __user *, shmaddr)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_shmdt, shmaddr);
#endif
}

SYSCALL_DEFINE2(host_mlock, unsigned long, start, size_t, len)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_mlock, start, len, 0, 0, 0, 0);
#else
	return host_syscall(__NR_mlock, start, len);
#endif
}

SYSCALL_DEFINE2(host_munlock, unsigned long, start, size_t, len)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_munlock, start, len, 0, 0, 0,
					  0);
#else
	return host_syscall(__NR_munlock, start, len);
#endif
}

SYSCALL_DEFINE1(host_mlockall, int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_mlockall, flags);
#endif
}

SYSCALL_DEFINE0(host_munlockall)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_munlockall);
#endif
}

SYSCALL_DEFINE3(host_mlock2, unsigned long, start, size_t, len, int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return flux_host_app_range_syscall(__NR_mlock2, start, len, flags, 0, 0,
					  0);
#else
	return host_syscall(__NR_mlock2, start, len, flags);
#endif
}

SYSCALL_DEFINE6(host_mbind, unsigned long, start, unsigned long, len,
		unsigned long, mode, const unsigned long __user *, nmask,
		unsigned long, maxnode, unsigned int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_mbind, start, len, mode, nmask, maxnode,
				    flags);
#endif
}

SYSCALL_DEFINE2(host_swapon, const char __user *, path, int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_swapon, path, flags);
#endif
}

SYSCALL_DEFINE1(host_swapoff, const char __user *, path)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_swapoff, path);
#endif
}

SYSCALL_DEFINE1(host_iopl, unsigned int, level)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_iopl, level);
#endif
}

SYSCALL_DEFINE3(host_ioperm, unsigned long, from, unsigned long, num, int,
		turn_on)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_ioperm, from, num, turn_on);
#endif
}

SYSCALL_DEFINE3(host_set_mempolicy, int, mode,
		const unsigned long __user *, nmask, unsigned long, maxnode)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_set_mempolicy, mode, nmask, maxnode);
#endif
}

SYSCALL_DEFINE5(host_get_mempolicy, int __user *, policy,
		unsigned long __user *, nmask, unsigned long, maxnode,
		unsigned long, addr, unsigned long, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_get_mempolicy, policy, nmask, maxnode, addr,
				    flags);
#endif
}

SYSCALL_DEFINE5(host_process_madvise, int, pidfd, unsigned long, vec,
		unsigned long, vlen, int, behavior, unsigned int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_process_madvise, pidfd, vec, vlen, behavior,
				    flags);
#endif
}

SYSCALL_DEFINE2(host_process_mrelease, int, pidfd, unsigned int, flags)
{
#ifdef CONFIG_FLUX_MPK
	return -EPERM;
#else
	return host_syscall(__NR_process_mrelease, pidfd, flags);
#endif
}

void flux_post_exec_to_user(void *entry, unsigned long stack)
{
	struct pt_regs *regs = current_pt_regs();
	int err;

	/*
	 * Flux bypasses the regular Linux exec path, so clear any TLS base
	 * inherited from the previous image before entering the new one.
	 */
	current->thread.fsbase = 0;
	wrfsbase(flux_host_fsbase());

	memset(regs, 0, sizeof(struct pt_regs));
	regs->sp = stack;
	regs->ip = (unsigned long)entry;

	err = flux_host_dev_clean_mm_post();
	BUG_ON(err);

	/* execve succeeded; fs/see exec.c */
	rseq_execve(current);
	user_events_execve(current);
	acct_update_integrals(current);
	task_numa_free(current, false);

	arch_exit_to_user_mode();
	syscall_ret_to_user(regs);
}

long flux_do_host_exec_with_post(const char *path, char **argv, char **envp,
				 flux_post_exec_fn_t post_exec)
{
	long retval;

again:
	if ((retval = flux_host_dev_clean_mm_pre()) < 0) {
		if (retval == -EAGAIN)
			goto again;
		return retval;
	}

	retval = flux_ops_load_elf(path, argv, envp, post_exec);

	/*
	 * If the execve fails, we need to abort the clean process
	 * to avoid leaking memory and deadlocking.
	 */
	flux_host_dev_clean_mm_abort();

	return retval;
}

long flux_do_host_exec(const char *path, char **argv, char **envp)
{
	return flux_do_host_exec_with_post(path, argv, envp,
					   flux_post_exec_to_user);
}

SYSCALL_DEFINE3(host_execve, const char __user *, path,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp)
{
	long retval;
	struct filename *filename;

	filename = getname(path);
	if (IS_ERR(filename))
		return PTR_ERR(filename);

	/*
	 * We move the actual failure in case of RLIMIT_NPROC excess from
	 * set*uid() to execve() because too many poorly written programs
	 * don't check setuid() return code.  Here we additionally recheck
	 * whether NPROC limit is still exceeded.
	 */
	if ((current->flags & PF_NPROC_EXCEEDED) &&
	    is_rlimit_overlimit(current_ucounts(), UCOUNT_RLIMIT_NPROC,
				rlimit(RLIMIT_NPROC))) {
		retval = -EAGAIN;
		putname(filename);
		return retval;
	}

	/* 
	 * We're below the limit (still or again), so we don't want to make
	 * further execve() calls fail.
	 */
	current->flags &= ~PF_NPROC_EXCEEDED;

	putname(filename);
	return flux_do_host_exec(path, (void *)argv, (void *)envp);
}

long flux_sys_mmap(unsigned long addr, unsigned long len, unsigned long prot,
		   unsigned long flags, unsigned long fd, unsigned long off)
{
	return sys_host_mmap(addr, len, prot, flags, fd, off);
}
