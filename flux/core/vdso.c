#define _GNU_SOURCE

#include <elf.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <flux.h>
#include <flux/mpk.h>

#include "vdso.h"

extern const unsigned char _binary_flux_vdso_so_start[];
extern const unsigned char _binary_flux_vdso_so_end[];

static void *flux_vdso_map;
static size_t flux_vdso_map_size;
static void *flux_vdso_region;
static size_t flux_vdso_region_size;
static void *flux_vvar_reader;
static void *flux_vvar_writer;
static const Elf64_Ehdr *flux_vdso_image;

static uintptr_t page_down(uintptr_t value, size_t page_size)
{
	return value & ~((uintptr_t)page_size - 1);
}

static uintptr_t page_up(uintptr_t value, size_t page_size)
{
	return (value + page_size - 1) & ~((uintptr_t)page_size - 1);
}

static int flux_vdso_validate(size_t image_size, size_t page_size,
			      size_t *map_size)
{
	const Elf64_Phdr *phdr;
	uintptr_t max_end = 0;
	size_t phdr_bytes;
	int load_count = 0;
	int i;

	if (image_size < sizeof(*flux_vdso_image))
		return -1;

	flux_vdso_image = (const Elf64_Ehdr *)_binary_flux_vdso_so_start;
	if (memcmp(flux_vdso_image->e_ident, ELFMAG, SELFMAG) ||
	    flux_vdso_image->e_ident[EI_CLASS] != ELFCLASS64 ||
	    flux_vdso_image->e_ident[EI_DATA] != ELFDATA2LSB ||
	    flux_vdso_image->e_type != ET_DYN ||
	    flux_vdso_image->e_machine != EM_X86_64 ||
	    flux_vdso_image->e_phentsize != sizeof(Elf64_Phdr))
		return -1;

	phdr_bytes = (size_t)flux_vdso_image->e_phnum * sizeof(Elf64_Phdr);
	if (flux_vdso_image->e_phoff > image_size ||
	    phdr_bytes > image_size - flux_vdso_image->e_phoff)
		return -1;

	phdr = (const Elf64_Phdr *)(_binary_flux_vdso_so_start +
				   flux_vdso_image->e_phoff);
	for (i = 0; i < flux_vdso_image->e_phnum; i++) {
		uintptr_t end;

		if (phdr[i].p_type != PT_LOAD)
			continue;
		load_count++;
		if (phdr[i].p_filesz > phdr[i].p_memsz ||
		    phdr[i].p_offset > image_size ||
		    phdr[i].p_filesz > image_size - phdr[i].p_offset ||
		    phdr[i].p_vaddr > UINTPTR_MAX - phdr[i].p_memsz ||
		    phdr[i].p_offset != 0 || phdr[i].p_vaddr != 0 ||
		    !(phdr[i].p_flags & PF_R) || !(phdr[i].p_flags & PF_X) ||
		    (phdr[i].p_flags & PF_W))
			return -1;

		end = phdr[i].p_vaddr + phdr[i].p_memsz;
		if (end > max_end)
			max_end = end;
	}

	if (load_count != 1 || !max_end || max_end > UINTPTR_MAX - page_size)
		return -1;

	*map_size = page_up(max_end, page_size);
	return 0;
}

static int flux_vdso_segment_prot(const Elf64_Phdr *phdr)
{
	int prot = PROT_READ;

	if (phdr->p_flags & PF_X)
		prot |= PROT_EXEC;
	return prot;
}

static int flux_vdso_apply_protection(bool mpk_shared)
{
	const Elf64_Phdr *phdr;
	long page_size_raw = sysconf(_SC_PAGESIZE);
	size_t page_size;
	int i;

	if (!flux_vdso_map || !flux_vdso_image || page_size_raw <= 0)
		return -1;
	page_size = (size_t)page_size_raw;
#ifdef CONFIG_FLUX_MPK
	if (mpk_shared &&
	    flux_mpk_protect_shared(flux_vvar_reader, page_size,
				    PROT_READ) < 0)
		return -1;
#endif

	phdr = (const Elf64_Phdr *)(_binary_flux_vdso_so_start +
				   flux_vdso_image->e_phoff);
	for (i = 0; i < flux_vdso_image->e_phnum; i++) {
		uintptr_t start, end;
		int prot;

		if (phdr[i].p_type != PT_LOAD || !phdr[i].p_memsz)
			continue;

		start = page_down((uintptr_t)flux_vdso_map + phdr[i].p_vaddr,
				  page_size);
		end = page_up((uintptr_t)flux_vdso_map + phdr[i].p_vaddr +
				      phdr[i].p_memsz,
			      page_size);
		prot = flux_vdso_segment_prot(&phdr[i]);

#ifdef CONFIG_FLUX_MPK
		if (mpk_shared) {
			if (flux_mpk_protect_shared((void *)start, end - start,
						    prot) < 0)
				return -1;
			continue;
		}
#else
		(void)mpk_shared;
#endif
		if (mprotect((void *)start, end - start, prot) < 0) {
			return -1;
		}
	}

	return 0;
}

int flux_vdso_init(void)
{
	const Elf64_Phdr *phdr;
	size_t image_size =
		(size_t)(_binary_flux_vdso_so_end - _binary_flux_vdso_so_start);
	long page_size_raw = sysconf(_SC_PAGESIZE);
	size_t page_size;
	void *map;
	int vvar_fd = -1;
	int i;

	if (flux_vdso_map)
		return 0;
	if (page_size_raw <= 0) {
		FLUX_LOG(FLUX_LOG_ERR, "invalid host page size\n");
		return -1;
	}
	page_size = (size_t)page_size_raw;
	if (flux_vdso_validate(image_size, page_size,
			       &flux_vdso_map_size) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "invalid embedded Flux vDSO image\n");
		return -1;
	}
	if (flux_vdso_map_size > SIZE_MAX - page_size) {
		FLUX_LOG(FLUX_LOG_ERR, "Flux vDSO mapping is too large\n");
		return -1;
	}
	flux_vdso_region_size = page_size + flux_vdso_map_size;

	vvar_fd = memfd_create("flux-vvar", MFD_CLOEXEC);
	if (vvar_fd < 0 || ftruncate(vvar_fd, (off_t)page_size) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to create Flux vvar: %s\n",
			 strerror(errno));
		goto out_unmap;
	}

	flux_vdso_region = mmap(NULL, flux_vdso_region_size, PROT_NONE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (flux_vdso_region == MAP_FAILED) {
		flux_vdso_region = NULL;
		FLUX_LOG(FLUX_LOG_ERR, "failed to reserve Flux vvar/vDSO: %s\n",
			 strerror(errno));
		goto out_unmap;
	}

	flux_vvar_reader = mmap(flux_vdso_region, page_size, PROT_READ,
				 MAP_SHARED | MAP_FIXED, vvar_fd, 0);
	if (flux_vvar_reader == MAP_FAILED) {
		flux_vvar_reader = NULL;
		FLUX_LOG(FLUX_LOG_ERR, "failed to map Flux vvar reader: %s\n",
			 strerror(errno));
		goto out_unmap;
	}

	flux_vvar_writer = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, vvar_fd, 0);
	if (flux_vvar_writer == MAP_FAILED) {
		flux_vvar_writer = NULL;
		FLUX_LOG(FLUX_LOG_ERR, "failed to map Flux vvar writer: %s\n",
			 strerror(errno));
		goto out_unmap;
	}
	close(vvar_fd);
	vvar_fd = -1;

	flux_vdso_map = (char *)flux_vdso_region + page_size;
	map = mmap(flux_vdso_map, flux_vdso_map_size,
		   PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (map == MAP_FAILED) {
		flux_vdso_map = NULL;
		FLUX_LOG(FLUX_LOG_ERR, "failed to map Flux vDSO: %s\n",
			 strerror(errno));
		goto out_unmap;
	}
	memset(flux_vdso_map, 0, flux_vdso_map_size);

	phdr = (const Elf64_Phdr *)(_binary_flux_vdso_so_start +
				   flux_vdso_image->e_phoff);
	for (i = 0; i < flux_vdso_image->e_phnum; i++) {
		void *dst;

		if (phdr[i].p_type != PT_LOAD || !phdr[i].p_filesz)
			continue;
		dst = (char *)flux_vdso_map + phdr[i].p_vaddr;
		memcpy(dst, _binary_flux_vdso_so_start + phdr[i].p_offset,
		       phdr[i].p_filesz);

		if ((phdr[i].p_flags & PF_X) &&
		    flux_mpk_scan_binary(dst, phdr[i].p_filesz, NULL) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "unsafe instruction in Flux vDSO text\n");
			goto out_unmap;
		}
	}

	if (mprotect(flux_vdso_map, flux_vdso_map_size, PROT_NONE) < 0 ||
	    flux_vdso_apply_protection(false) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to protect Flux vDSO: %s\n",
			 strerror(errno));
		goto out_unmap;
	}

	FLUX_LOG(FLUX_LOG_INFO,
		 "Flux vvar reader %p writer %p, vDSO %p (%zu bytes)\n",
		 flux_vvar_reader, flux_vvar_writer, flux_vdso_map,
		 flux_vdso_map_size);
	return 0;

out_unmap:
	if (vvar_fd >= 0)
		close(vvar_fd);
	if (flux_vvar_writer)
		munmap(flux_vvar_writer, page_size);
	if (flux_vdso_region)
		munmap(flux_vdso_region, flux_vdso_region_size);
	flux_vdso_map = NULL;
	flux_vdso_map_size = 0;
	flux_vdso_region = NULL;
	flux_vdso_region_size = 0;
	flux_vvar_reader = NULL;
	flux_vvar_writer = NULL;
	flux_vdso_image = NULL;
	return -1;
}

#ifdef CONFIG_FLUX_MPK
int flux_vdso_protect_shared(void)
{
	if (flux_vdso_apply_protection(true) < 0) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to assign shared MPK key to Flux vDSO: %s\n",
			 strerror(errno));
		return -1;
	}
	return 0;
}
#endif

void flux_vdso_fini(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	if (flux_vvar_writer && page_size > 0)
		munmap(flux_vvar_writer, (size_t)page_size);
	if (flux_vdso_region)
		munmap(flux_vdso_region, flux_vdso_region_size);
	flux_vdso_map = NULL;
	flux_vdso_map_size = 0;
	flux_vdso_region = NULL;
	flux_vdso_region_size = 0;
	flux_vvar_reader = NULL;
	flux_vvar_writer = NULL;
	flux_vdso_image = NULL;
}

void *flux_vdso_ehdr(void)
{
	return flux_vdso_map;
}

void *flux_vdso_data(void)
{
	return flux_vvar_writer;
}
