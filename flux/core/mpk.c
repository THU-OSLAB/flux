#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <flux.h>
#include <flux/mpk.h>

static int flux_app_pkey = -1;
static int flux_shared_pkey = -1;

_Static_assert(sizeof(struct flux_rodata) == FLUX_RODATA_HEADER_SIZE,
	       "syscall entry mapping must occupy exactly one page");

static inline bool flux_mpk_is_wrpkru(const unsigned char *p)
{
	return p[0] == 0x0f && p[1] == 0x01 && p[2] == 0xef;
}

static inline bool flux_mpk_is_xrstor(const unsigned char *p)
{
	return p[0] == 0x0f && p[1] == 0xae && (p[2] & 0xc0) != 0xc0 &&
	       (p[2] & 0x38) == 0x28;
}

static inline bool flux_mpk_is_xrstors(const unsigned char *p)
{
	return p[0] == 0x0f && p[1] == 0xc7 && (p[2] & 0xc0) != 0xc0 &&
	       (p[2] & 0x38) == 0x18;
}

int flux_mpk_scan_binary(const void *addr, size_t len, size_t *offset)
{
	const unsigned char *p = addr;
	size_t i;

	if (len < 3)
		return 0;

	for (i = 0; i <= len - 3; i++) {
		if (flux_mpk_is_wrpkru(p + i) || flux_mpk_is_xrstor(p + i) ||
		    flux_mpk_is_xrstors(p + i)) {
			if (offset)
				*offset = i;
			return -FLUX_ENOEXEC;
		}
	}

	return 0;
}

int flux_mpk_disable_host_rseq(void)
{
#ifdef RSEQ_SIG
	struct rseq *rseq;

	/*
	 * Host rseq bookkeeping runs on interrupt return with the interrupted
	 * PKRU. Once the application domain is active, the host kernel cannot
	 * update glibc's pkey-0 TLS rseq area. Unregister it before lowering
	 * privilege; guest rseq remains independently managed by Flux.
	 */
	if (!__rseq_size)
		return 0;

	rseq = (void *)((char *)__builtin_thread_pointer() + __rseq_offset);
	if (syscall(SYS_rseq, rseq, __rseq_size, RSEQ_FLAG_UNREGISTER,
		    RSEQ_SIG) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to unregister host rseq: %s\n",
			 strerror(errno));
		return -1;
	}

	/* Make glibc and other rseq users take their non-rseq fallback. */
	rseq->cpu_id = RSEQ_CPU_ID_REGISTRATION_FAILED;
#endif
	return 0;
}

int flux_mpk_protect_app(void *addr, size_t len, int prot)
{
	uintptr_t start, end;
	long pagesz;

	if (flux_app_pkey < 0 || !len)
		return 0;

	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz <= 0 || (uintptr_t)addr > UINTPTR_MAX - len) {
		errno = EINVAL;
		return -1;
	}

	start = (uintptr_t)addr & ~((uintptr_t)pagesz - 1);
	end = ((uintptr_t)addr + len + pagesz - 1) &
	      ~((uintptr_t)pagesz - 1);
	if (syscall(SYS_pkey_mprotect, (void *)start, end - start, prot,
		    flux_app_pkey) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "pkey_mprotect app range [%p, %p) failed: %s\n",
			 (void *)start, (void *)end, strerror(errno));
		return -1;
	}

	return 0;
}

int flux_mpk_protect_shared(void *addr, size_t len, int prot)
{
	if (flux_shared_pkey < 0) {
		errno = EINVAL;
		return -1;
	}

	if (syscall(SYS_pkey_mprotect, addr, len, prot, flux_shared_pkey) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "pkey_mprotect shared range [%p, %p) failed: %s\n",
			 addr, (char *)addr + len, strerror(errno));
		return -1;
	}

	return 0;
}

static int flux_mpk_alloc_fixed(int expected, const char *domain)
{
	int pkey = syscall(SYS_pkey_alloc, 0, 0);

	if (pkey == expected)
		return pkey;
	if (pkey >= 0)
		syscall(SYS_pkey_free, pkey);

	FLUX_LOG(FLUX_LOG_ERR,
		 "%s pkey allocation returned %d, expected fixed key %d\n",
		 domain, pkey, expected);
	errno = pkey < 0 ? errno : EBUSY;
	return -1;
}

int flux_mpk_init(void)
{
	struct flux_rodata *rodata = (void *)FLUX_RODATA_ADDR;

	flux_app_pkey = flux_mpk_alloc_fixed(FLUX_MPK_APP_PKEY, "application");
	if (flux_app_pkey < 0)
		return -1;

	flux_shared_pkey =
		flux_mpk_alloc_fixed(FLUX_MPK_SHARED_PKEY, "shared");
	if (flux_shared_pkey < 0)
		goto out_free_app;

	if (syscall(SYS_pkey_mprotect, rodata, FLUX_RODATA_HEADER_SIZE,
		    PROT_READ, flux_shared_pkey) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to protect syscall entry page: %s\n",
			 strerror(errno));
		goto out_free_shared;
	}

	FLUX_LOG(FLUX_LOG_INFO,
		 "MPK domains: kernel key %d, app key %d, shared key %d, app PKRU %#x\n",
		 FLUX_MPK_KERNEL_PKEY, flux_app_pkey, flux_shared_pkey,
		 FLUX_MPK_APP_PKRU);
	return 0;

out_free_shared:
	syscall(SYS_pkey_free, flux_shared_pkey);
	flux_shared_pkey = -1;
out_free_app:
	syscall(SYS_pkey_free, flux_app_pkey);
	flux_app_pkey = -1;
	return -1;
}
