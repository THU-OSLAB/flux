#ifndef _ASM_FLUX_HOST_OPS_H
#define _ASM_FLUX_HOST_OPS_H

#include <uapi/asm/host_ops.h>
#include <linux/thread_info.h>

extern struct flux_host_operations *flux_ops;
extern unsigned long *flux_host_fsbases;

#define FLUX_CLONE_FLAGS                                                    \
	(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD | CLONE_SIGHAND | \
	 SIGCHLD)

#include <asm/current.h>
#include <asm/processor.h>
#include <asm/smp.h>

#define __FLUX_MAP0(m, ...)
#define __FLUX_MAP1(m, t, a) m(t, a)
#define __FLUX_MAP2(m, t, a, ...) m(t, a), __FLUX_MAP1(m, __VA_ARGS__)
#define __FLUX_MAP3(m, t, a, ...) m(t, a), __FLUX_MAP2(m, __VA_ARGS__)
#define __FLUX_MAP4(m, t, a, ...) m(t, a), __FLUX_MAP3(m, __VA_ARGS__)
#define __FLUX_MAP5(m, t, a, ...) m(t, a), __FLUX_MAP4(m, __VA_ARGS__)
#define __FLUX_MAP6(m, t, a, ...) m(t, a), __FLUX_MAP5(m, __VA_ARGS__)
#define __FLUX_MAP7(m, t, a, ...) m(t, a), __FLUX_MAP6(m, __VA_ARGS__)
#define __FLUX_MAP8(m, t, a, ...) m(t, a), __FLUX_MAP7(m, __VA_ARGS__)
#define __FLUX_MAP9(m, t, a, ...) m(t, a), __FLUX_MAP8(m, __VA_ARGS__)
#define __FLUX_MAP(n, ...) __FLUX_MAP##n(__VA_ARGS__)

#define __FLUX_SC_DECL(t, a) t a
#define __FLUX_SC_ARGS(t, a) a

static inline u64 flux_host_fsbase(void)
{
#ifdef CONFIG_SMP
	return flux_host_fsbases[raw_smp_processor_id()];
#else
	return flux_host_fsbases[0];
#endif
}

static inline u64 flux_host_call_save_fsbase(void)
{
	unsigned long fsbase;

	asm volatile("rdfsbase %0" : "=r"(fsbase));

	return fsbase;
}

static inline void flux_host_call_restore_fsbase(u64 fsbase)
{
	asm volatile("wrfsbase %0" : : "r"(fsbase));
}


#define FLUX_HOST_CALL_BEGIN(__flags, __fsbase)            \
	u64 __fsbase = flux_host_call_save_fsbase();       \
	flux_host_call_restore_fsbase(flux_host_fsbase())

#define FLUX_HOST_CALL_END(__flags, __fsbase) \
	flux_host_call_restore_fsbase(__fsbase)

#define FLUX_HOST_CALL_VOID0(__ops, __fn)                          \
	static __always_inline void __ops##_##__fn(void)           \
	{                                                          \
		FLUX_HOST_CALL_BEGIN(__flags, __fsbase);           \
		__ops->__fn();                                     \
		FLUX_HOST_CALL_END(__flags, __fsbase);             \
	}                                                          \
	static __always_inline void __ops##_##__fn##_raw(void)     \
	{                                                          \
		__ops->__fn();                                     \
	}

#define FLUX_HOST_CALL_VOIDx(x, __ops, __fn, ...)                        \
	static __always_inline void __ops##_##__fn(                      \
		__FLUX_MAP(x, __FLUX_SC_DECL, __VA_ARGS__))              \
	{                                                                \
		FLUX_HOST_CALL_BEGIN(__flags, __fsbase);                 \
		__ops->__fn(__FLUX_MAP(x, __FLUX_SC_ARGS, __VA_ARGS__)); \
		FLUX_HOST_CALL_END(__flags, __fsbase);                   \
	}                                                                \
	static __always_inline void __ops##_##__fn##_raw(                \
		__FLUX_MAP(x, __FLUX_SC_DECL, __VA_ARGS__))              \
	{                                                                \
		__ops->__fn(__FLUX_MAP(x, __FLUX_SC_ARGS, __VA_ARGS__)); \
	}

#define FLUX_HOST_CALL0(__ops, __fn, __rettype)                     \
	static __always_inline __rettype __ops##_##__fn(void)       \
	{                                                           \
		__rettype __ret;                                    \
		FLUX_HOST_CALL_BEGIN(__flags, __fsbase);            \
		__ret = __ops->__fn();                              \
		FLUX_HOST_CALL_END(__flags, __fsbase);              \
		return __ret;                                       \
	}                                                           \
	static __always_inline __rettype __ops##_##__fn##_raw(void) \
	{                                                           \
		return __ops->__fn();                               \
	}

#define FLUX_HOST_CALLx(x, __ops, __fn, __rettype, ...)              \
	static __always_inline __rettype __ops##_##__fn(             \
		__FLUX_MAP(x, __FLUX_SC_DECL, __VA_ARGS__))          \
	{                                                            \
		__rettype __ret;                                     \
		FLUX_HOST_CALL_BEGIN(__flags, __fsbase);             \
		__ret = __ops->__fn(                                 \
			__FLUX_MAP(x, __FLUX_SC_ARGS, __VA_ARGS__)); \
		FLUX_HOST_CALL_END(__flags, __fsbase);               \
		return __ret;                                        \
	}                                                            \
	static __always_inline __rettype __ops##_##__fn##_raw(       \
		__FLUX_MAP(x, __FLUX_SC_DECL, __VA_ARGS__))          \
	{                                                            \
		return __ops->__fn(                                  \
			__FLUX_MAP(x, __FLUX_SC_ARGS, __VA_ARGS__)); \
	}


typedef void (*flux_thread_fn_t)(void *);

FLUX_HOST_CALL_VOIDx(2, flux_ops, print, const char *, str, int, len);
FLUX_HOST_CALL_VOID0(flux_ops, panic);
FLUX_HOST_CALLx(1, flux_ops, sem_alloc, struct flux_sem *, int, count);
FLUX_HOST_CALL_VOIDx(1, flux_ops, sem_free, struct flux_sem *, sem);
FLUX_HOST_CALL_VOIDx(1, flux_ops, sem_up, struct flux_sem *, sem);
FLUX_HOST_CALL_VOIDx(1, flux_ops, sem_down, struct flux_sem *, sem);
FLUX_HOST_CALLx(1, flux_ops, mutex_alloc, struct flux_mutex *, int, recursive);
FLUX_HOST_CALL_VOIDx(1, flux_ops, mutex_free, struct flux_mutex *, mutex);
FLUX_HOST_CALL_VOIDx(1, flux_ops, mutex_lock, struct flux_mutex *, mutex);
FLUX_HOST_CALL_VOIDx(1, flux_ops, mutex_unlock, struct flux_mutex *, mutex);
FLUX_HOST_CALLx(3, flux_ops, thread_create, flux_thread_t, flux_thread_fn_t, f,
		void *, arg, char *, name);
FLUX_HOST_CALL_VOID0(flux_ops, thread_detach);
FLUX_HOST_CALL_VOID0(flux_ops, thread_exit);
FLUX_HOST_CALL_VOIDx(1, flux_ops, thread_longjmp, int, cpu);
FLUX_HOST_CALLx(1, flux_ops, thread_join, int, flux_thread_t, tid);
FLUX_HOST_CALL0(flux_ops, thread_self, flux_thread_t);
FLUX_HOST_CALLx(2, flux_ops, thread_equal, int, flux_thread_t, a, flux_thread_t,
		b);
FLUX_HOST_CALLx(1, flux_ops, thread_stack, void *, unsigned long *, size);
FLUX_HOST_CALLx(1, flux_ops, thread_bind, int, int, cpu);
FLUX_HOST_CALLx(1, flux_ops, mem_alloc, void *, unsigned long, size);
FLUX_HOST_CALL_VOIDx(1, flux_ops, mem_free, void *, addr);
FLUX_HOST_CALLx(4, flux_ops, page_alloc, void *, void *, hint, unsigned long,
		size, unsigned long, align, int, flags);
FLUX_HOST_CALL_VOIDx(2, flux_ops, page_free, void *, addr, unsigned long, size);
FLUX_HOST_CALLx(1, flux_ops, page_handle, int, void *, addr);
FLUX_HOST_CALLx(1, flux_ops, va_to_pa, void *, void *, addr);
FLUX_HOST_CALL0(flux_ops, time, unsigned long long);
FLUX_HOST_CALLx(1, flux_ops, timer_alloc, void *, struct flux_timer_args *,
		args);
FLUX_HOST_CALLx(2, flux_ops, timer_set_oneshot, int, void *, timer,
		unsigned long, delta);
FLUX_HOST_CALL_VOIDx(1, flux_ops, timer_free, void *, timer);
FLUX_HOST_CALLx(2, flux_ops, ioremap, void *, long, addr, int, size);
FLUX_HOST_CALLx(4, flux_ops, iomem_access, int, const volatile void *, addr,
		void *, val, int, size, int, write);
FLUX_HOST_CALL0(flux_ops, gettid, long);
FLUX_HOST_CALL0(flux_ops, getcpu, int);
FLUX_HOST_CALL_VOID0(flux_ops, yield);
FLUX_HOST_CALLx(2, flux_ops, uintr_register_ipi, int, int, cpu, int, vector);
FLUX_HOST_CALLx(3, flux_ops, handle_mpk_fault, int, int, sig, void *, ucontext,
		struct flux_mpk_cmp64 *, cmp);
FLUX_HOST_CALLx(2, flux_ops, rewrite_exec, int, void *, addr, unsigned long,
		len);
FLUX_HOST_CALLx(3, flux_ops, invalidate_exec, int, void *, addr,
		unsigned long, len, bool, restore);

#ifdef CONFIG_FLUX_SPDK
#include <uapi/asm/spdk.h>

extern struct flux_spdk *flux_spdk;
extern struct flux_spdk_operations *flux_spdk_ops;

FLUX_HOST_CALLx(1, flux_spdk_ops, cpl_is_error, int,
		const struct spdk_nvme_cpl *, cpl);
FLUX_HOST_CALLx(1, flux_spdk_ops, ns_get_size, uint64_t, struct spdk_nvme_ns *,
		ns);
FLUX_HOST_CALLx(1, flux_spdk_ops, ns_get_sector_size, uint32_t,
		struct spdk_nvme_ns *, ns);
FLUX_HOST_CALLx(8, flux_spdk_ops, ns_cmd_write, int, struct spdk_nvme_ns *, ns,
		struct spdk_nvme_qpair *, qpair, void *, payload, uint64_t, lba,
		uint32_t, lba_count, spdk_nvme_cmd_cb, cb_fn, void *, cb_arg,
		uint32_t, io_flags);
FLUX_HOST_CALLx(9, flux_spdk_ops, ns_cmd_writev, int, struct spdk_nvme_ns *, ns,
		struct spdk_nvme_qpair *, qpair, uint64_t, lba, uint32_t,
		lba_count, spdk_nvme_cmd_cb, cb_fn, void *, cb_arg, uint32_t,
		io_flags, spdk_nvme_req_reset_sgl_cb, reset_sgl_fn,
		spdk_nvme_req_next_sge_cb, next_sge_fn);
FLUX_HOST_CALLx(8, flux_spdk_ops, ns_cmd_read, int, struct spdk_nvme_ns *, ns,
		struct spdk_nvme_qpair *, qpair, void *, payload, uint64_t, lba,
		uint32_t, lba_count, spdk_nvme_cmd_cb, cb_fn, void *, cb_arg,
		uint32_t, io_flags);
FLUX_HOST_CALLx(9, flux_spdk_ops, ns_cmd_readv, int, struct spdk_nvme_ns *, ns,
		struct spdk_nvme_qpair *, qpair, uint64_t, lba, uint32_t,
		lba_count, spdk_nvme_cmd_cb, cb_fn, void *, cb_arg, uint32_t,
		io_flags, spdk_nvme_req_reset_sgl_cb, reset_sgl_fn,
		spdk_nvme_req_next_sge_cb, next_sge_fn);
FLUX_HOST_CALLx(1, flux_spdk_ops, mempool_get, void *, struct spdk_mempool *,
		mp);
FLUX_HOST_CALL_VOIDx(2, flux_spdk_ops, mempool_put, struct spdk_mempool *, mp,
		     void *, buf);
FLUX_HOST_CALLx(2, flux_spdk_ops, qpair_process_completions, int,
		struct spdk_nvme_qpair *, qpair, uint32_t, timeout);
#endif

#endif
