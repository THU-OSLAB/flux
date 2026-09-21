#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <flux.h>
#include <flux/mpk.h>

static int flux_app_pkey = -1;
static int flux_shared_pkey = -1;

_Static_assert(sizeof(struct flux_rodata) == FLUX_RODATA_HEADER_SIZE,
	       "syscall entry mapping must occupy exactly one page");
_Static_assert(CONFIG_FLUX_MAX_CPUS <= FLUX_MPK_RETURN_SLOT_CAPACITY,
	       "configured CPUs exceed the shared return-slot capacity");

int flux_mpk_protect_kernel(void *addr, size_t len, int prot)
{
	uintptr_t start, end, unaligned_end;
	long pagesz;

	if (!len)
		return 0;
	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz <= 0 || (uintptr_t)addr > UINTPTR_MAX - len) {
		errno = EINVAL;
		return -1;
	}
	unaligned_end = (uintptr_t)addr + len;
	if (unaligned_end > UINTPTR_MAX - ((uintptr_t)pagesz - 1)) {
		errno = EINVAL;
		return -1;
	}

	start = (uintptr_t)addr & ~((uintptr_t)pagesz - 1);
	end = (unaligned_end + pagesz - 1) &
	      ~((uintptr_t)pagesz - 1);
	if (syscall(SYS_pkey_mprotect, (void *)start, end - start, prot,
		    FLUX_MPK_KERNEL_PKEY) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "pkey_mprotect kernel range [%p, %p) failed: %s\n",
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
	unsigned int cpu;

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
	if (syscall(SYS_pkey_mprotect, (void *)FLUX_MPK_RETURN_ADDR,
		    FLUX_MPK_RETURN_AREA_SIZE, PROT_READ | PROT_WRITE,
		    flux_shared_pkey) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to protect MPK return slots: %s\n",
			 strerror(errno));
		goto out_free_shared;
	}

	/* Keep return slots and flags read-only to the application. Only the
	 * separate pages below the flags words receive writable UINTR frames.
	 */
	for (cpu = 0; cpu < CONFIG_FLUX_MAX_CPUS; cpu++) {
		void *landing = (void *)(uintptr_t)(FLUX_SYSCALL_FLAGS_STACK_ADDR +
				cpu * FLUX_SYSCALL_FLAGS_STACK_SIZE);

		if (syscall(SYS_pkey_mprotect, landing, FLUX_MPK_RETURN_PAGE_SIZE,
			    PROT_READ | PROT_WRITE, flux_app_pkey) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to protect return UINTR landing page: %s\n",
				 strerror(errno));
			goto out_free_shared;
		}
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
