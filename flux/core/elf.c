/*
 * ELF Loader with PT_INTERP
 */
#define FLUX_FMT "elf: "

#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/auxvec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <flux.h>
#include <flux/mpk.h>
#include <flux/runc.h>

#include "elf.h"
#include "io/iok_ext.h"
#include "vdso.h"

#ifndef PR_SET_MM
#define PR_SET_MM 35
#endif

#ifndef PR_SET_MM_EXE_FILE
#define PR_SET_MM_EXE_FILE 13
#endif

static inline void *flux_sys_mmap_checked(void *addr, size_t len, int prot,
					  int flags, int fd, unsigned long off)
{
	long ret =
		flux_sys_mmap((unsigned long)addr, len, prot, flags, fd, off);

	if (ret < 0)
		return MAP_FAILED;

	return (void *)ret;
}

#define FLUX_SCRIPT_LINE_BUF_SIZE 256

static inline bool flux_spacetab(char c)
{
	return c == ' ' || c == '\t';
}

static void flux_free_argv_copy(char **argv)
{
	size_t i = 0;

	if (!argv)
		return;

	while (argv[i])
		free(argv[i++]);
	free(argv);
}

/*
 * Parse a shebang line in a script file.
 * Returns:
 *   0             script with valid "#!" line parsed
 *   -FLUX_ENOEXEC not a script or invalid/truncated shebang
 *   <0            hard errors (I/O, OOM)
 */
static int flux_parse_script_shebang(const char *file, char **out_interp,
				     char **out_interp_arg)
{
	char buf[FLUX_SCRIPT_LINE_BUF_SIZE];
	const char *buf_end, *i_name, *i_sep, *i_arg, *i_end, *scan;
	long nread;
	int fd;

	*out_interp = NULL;
	*out_interp_arg = NULL;

	fd = flux_sys_open(file, FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n", file);
		return -FLUX_EBADF;
	}

	nread = flux_sys_read(fd, buf, sizeof(buf) - 1);
	flux_sys_close(fd);
	if (nread < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to read %s\n", file);
		return -FLUX_EBADF;
	}

	buf[nread] = '\0';

	if (nread < 2 || buf[0] != '#' || buf[1] != '!')
		return -FLUX_ENOEXEC;

	/*
	 * Keep parser behavior close to fs/binfmt_script.c:
	 * reject truncated interpreter paths when no newline is present.
	 */
	buf_end = buf + nread;
	i_end = memchr(buf, '\n', nread);
	if (!i_end) {
		scan = buf + 2;
		while (scan < buf_end && flux_spacetab(*scan))
			scan++;
		if (scan >= buf_end)
			return -FLUX_ENOEXEC;
		while (scan < buf_end && !flux_spacetab(*scan) && *scan)
			scan++;
		if (scan >= buf_end)
			return -FLUX_ENOEXEC;
		i_end = buf_end;
	}

	while (i_end > buf + 2 && flux_spacetab(i_end[-1]))
		i_end--;

	i_name = buf + 2;
	while (i_name < i_end && flux_spacetab(*i_name))
		i_name++;
	if (!i_name || i_name == i_end)
		return -FLUX_ENOEXEC;

	i_sep = i_name;
	while (i_sep < i_end && !flux_spacetab(*i_sep) && *i_sep)
		i_sep++;

	*out_interp = strndup(i_name, i_sep - i_name);
	if (!*out_interp)
		return -FLUX_ENOMEM;

	i_arg = NULL;
	if (i_sep < i_end)
		i_arg = i_sep;
	if (i_arg) {
		while (i_arg < i_end && flux_spacetab(*i_arg))
			i_arg++;
		if (i_arg < i_end) {
			*out_interp_arg = strndup(i_arg, i_end - i_arg);
			if (!*out_interp_arg) {
				free(*out_interp);
				*out_interp = NULL;
				return -FLUX_ENOMEM;
			}
		}
	}

	return 0;
}

static int flux_build_script_argv(int argc, char **argv,
				  const char *script_path,
				  const char *interp_path,
				  const char *interp_arg, int *out_argc,
				  char ***out_argv)
{
	char **new_argv = NULL;
	int new_argc;
	int i, pos = 0;

	new_argc = argc + 1 + (interp_arg ? 1 : 0);
	new_argv = calloc((size_t)new_argc + 1, sizeof(*new_argv));
	if (!new_argv)
		return -FLUX_ENOMEM;

	new_argv[pos] = strdup(interp_path);
	if (!new_argv[pos++])
		goto out_err;

	if (interp_arg) {
		new_argv[pos] = strdup(interp_arg);
		if (!new_argv[pos++])
			goto out_err;
	}

	new_argv[pos] = strdup(script_path);
	if (!new_argv[pos++])
		goto out_err;

	for (i = 1; i < argc; i++) {
		new_argv[pos] = strdup(argv[i]);
		if (!new_argv[pos++])
			goto out_err;
	}

	new_argv[pos] = NULL;
	*out_argc = new_argc;
	*out_argv = new_argv;
	return 0;

out_err:
	flux_free_argv_copy(new_argv);
	return -FLUX_ENOMEM;
}

static void flux_set_proc_self_exe(const char *file)
{
#ifdef __flux__NR_prctl
	int fd;
	long ret;

	if (!file || !file[0])
		return;

	fd = flux_sys_open(file, FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_DEBUG,
			 "failed to open %s for PR_SET_MM_EXE_FILE: %s\n", file,
			 flux_strerror(fd));
		return;
	}

	ret = flux_sys_prctl(PR_SET_MM, PR_SET_MM_EXE_FILE, (unsigned long)fd,
			     0, 0);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_DEBUG,
			 "prctl(PR_SET_MM_EXE_FILE, %s) failed: %s\n", file,
			 flux_strerror(ret));
	}

	flux_sys_close(fd);
#else
	(void)file;
#endif
}

struct flux_vma_range {
	elf_addr_t start;
	elf_addr_t end;
};

#define ELF_ET_DYN_BASE (((1UL << 47) - PGSIZE_4KB) / 3 * 2)
#define ELF_ET_DYN_MIN_ADDR 0x200000000000UL

static bool flux_et_dyn_base_fits(struct flux_vma_range window, elf_addr_t base,
				  elf_addr_t map_start, elf_addr_t total_sz)
{
	elf_addr_t reserve_start = base + map_start;
	elf_addr_t reserve_end = reserve_start + total_sz;

	return reserve_start >= window.start && reserve_end <= window.end;
}

static void *flux_try_reserve_et_dyn_base(elf_addr_t base, elf_addr_t map_start,
					  elf_addr_t total_sz,
					  int reserve_flags)
{
	void *want = (void *)(base + map_start);
	void *rsvd;

	rsvd = flux_sys_mmap_checked(want, total_sz, FLUX_PROT_NONE,
				     reserve_flags, -1, 0);
	if (rsvd == want)
		return rsvd;

	if (rsvd != MAP_FAILED)
		munmap(rsvd, total_sz);

	return MAP_FAILED;
}

static int flux_reserve_et_dyn_region(elf_addr_t map_start, elf_addr_t total_sz,
				      void **rsvd_out)
{
#ifdef MAP_FIXED_NOREPLACE
	/*
	 * Keep ET_DYN mappings away from Flux's fixed windows, but bias the
	 * search around Linux's usual ELF_ET_DYN_BASE so PIE/brk layout stays
	 * closer to what user space expects.
	 */
	static const struct flux_vma_range dyn_windows[] = {
		{
			.start = FLUX_IOK_CLIENT_WINDOW_BASE +
				 FLUX_IOK_CLIENT_WINDOW_SIZE,
			.end = FLUX_MEMORY_ADDR,
		},
		{
			.start = ELF_ET_DYN_MIN_ADDR,
			.end = FLUX_IOK_CLIENT_WINDOW_BASE,
		},
	};
	int reserve_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
	elf_addr_t stride =
		align_up(MAX(total_sz, (elf_addr_t)PGSIZE_2MB), PGSIZE_2MB);
	int i, pass;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0;
		     i < (int)(sizeof(dyn_windows) / sizeof(dyn_windows[0]));
		     i++) {
			elf_addr_t lo;
			elf_addr_t hi;
			elf_addr_t anchor;
			elf_addr_t delta;

			if (dyn_windows[i].end < map_start + total_sz)
				continue;

			if (dyn_windows[i].start < map_start)
				lo = 0;
			else
				lo = align_up(dyn_windows[i].start - map_start,
					      PGSIZE_4KB);

			hi = align_down(dyn_windows[i].end - map_start -
						total_sz,
					PGSIZE_4KB);
			if (hi < lo)
				continue;

			if (pass == 0) {
				if (ELF_ET_DYN_BASE < lo)
					anchor = lo;
				else if (ELF_ET_DYN_BASE > hi)
					anchor = hi;
				else
					anchor = align_down(ELF_ET_DYN_BASE,
							    PGSIZE_4KB);
			} else {
				anchor = hi;
			}

			for (delta = 0;; delta += stride) {
				bool can_try_down = delta <= anchor - lo;
				bool can_try_up = delta && delta <= hi - anchor;
				void *rsvd;

				if (can_try_down) {
					elf_addr_t base = anchor - delta;

					if (flux_et_dyn_base_fits(
						    dyn_windows[i], base,
						    map_start, total_sz)) {
						rsvd = flux_try_reserve_et_dyn_base(
							base, map_start,
							total_sz,
							reserve_flags);
						if (rsvd != MAP_FAILED) {
							*rsvd_out = rsvd;
							return 0;
						}
					}
				}

				if (!delta)
					goto next_delta;

				if (can_try_up) {
					elf_addr_t base = anchor + delta;

					if (flux_et_dyn_base_fits(
						    dyn_windows[i], base,
						    map_start, total_sz)) {
						rsvd = flux_try_reserve_et_dyn_base(
							base, map_start,
							total_sz,
							reserve_flags);
						if (rsvd != MAP_FAILED) {
							*rsvd_out = rsvd;
							return 0;
						}
					}
				}

next_delta:
				if (!can_try_down && !can_try_up)
					break;
			}
		}
	}
	return -FLUX_ENOMEM;
#else
	return -FLUX_ENOSYS;
#endif
}

static int flux_find_interp(const char *file, char **interp_path)
{
	const char *interp = NULL;
	struct flux_stat st;
	Elf64_Ehdr *eh;
	Elf64_Phdr *ph;
	void *addr;
	int fd, i, err = 0;

	if (run_cfg && run_cfg->ld_path && run_cfg->ld_path[0]) {
		*interp_path = strdup(run_cfg->ld_path);
		if (!*interp_path) {
			FLUX_LOG(
				FLUX_LOG_ERR,
				"failed to copy configured interp from ld_path %s\n",
				run_cfg->ld_path);
			return -FLUX_ENOMEM;
		}

		FLUX_LOG(FLUX_LOG_DEBUG, "using interp from ld_path: %s\n",
			 *interp_path);
		return 0;
	}

	fd = flux_sys_open(file, FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n", file);
		err = -FLUX_EBADF;
		goto out;
	}

	if (flux_sys_fstat(fd, &st) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to stat %s\n", file);
		err = -FLUX_EBADF;
		goto out_close;
	}

	addr = flux_sys_mmap_checked(NULL, st.st_size, FLUX_PROT_READ,
				     MAP_PRIVATE, fd, 0);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mmap %s\n", file);
		err = -FLUX_EFAULT;
		goto out_close;
	}

	eh = (Elf64_Ehdr *)addr;
	ph = (Elf64_Phdr *)(addr + eh->e_phoff);

	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_INTERP) {
			interp = (const char *)(addr + ph[i].p_offset);
			break;
		}
	}
	if (!interp) {
		FLUX_LOG(FLUX_LOG_ERR, "no PT_INTERP found in %s\n", file);
		err = -FLUX_ENOEXEC;
		goto out_munmap;
	}

	*interp_path = interp ? strdup(interp) : NULL;
	if (!*interp_path) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to copy PT_INTERP from %s\n",
			 file);
		err = -FLUX_ENOMEM;
	}

out_munmap:
	munmap(addr, st.st_size);
out_close:
	flux_sys_close(fd);
out:
	return err;
}

static bool flux_check_ehdr(Elf64_Ehdr *eh)
{
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		return false;
	if (eh->e_ident[EI_CLASS] != ELFCLASS64)
		return false;
	if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
		return false;
	if (eh->e_machine != EM_X86_64)
		return false;
	return true;
}

static inline int flux_prot_from_pflags(Elf64_Word pf)
{
	int prot = 0;
	if (pf & PF_R)
		prot |= FLUX_PROT_READ;
	if (pf & PF_W)
		prot |= FLUX_PROT_WRITE;
	if (pf & PF_X)
		prot |= FLUX_PROT_EXEC;
	return prot;
}

static int __load_elf(const char *path, struct flux_elf_info *info,
		      bool scan_binary)
{
	struct flux_stat st;
	Elf64_Ehdr *eh;
	Elf64_Phdr *ph;
	void *addr, *rsvd, *base;
	elf_addr_t map_start, map_end, total_sz;
	elf_addr_t min_vaddr, max_vaddr;
	int reserve_flags = MAP_PRIVATE | MAP_ANONYMOUS;
	bool did_reserve = false;
	int fd, i, err = 0;

	fd = flux_sys_open(path, FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s\n", path);
		err = -FLUX_EBADF;
		goto out;
	}

	if (flux_sys_fstat(fd, &st) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to fstat %s\n", path);
		err = -FLUX_EBADF;
		goto out_close;
	}

	addr = flux_sys_mmap_checked(NULL, st.st_size, FLUX_PROT_READ,
				     MAP_PRIVATE, fd, 0);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mmap %s\n", path);
		err = -FLUX_ENOMEM;
		goto out_close;
	}

	eh = (Elf64_Ehdr *)addr;
	if (!flux_check_ehdr(eh)) {
		FLUX_LOG(FLUX_LOG_ERR, "bad elf header %s\n", path);
		err = -FLUX_ENOEXEC;
		goto out_munmap;
	}

	ph = (Elf64_Phdr *)(addr + eh->e_phoff);
	min_vaddr = UINT64_MAX;
	max_vaddr = 0;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_vaddr < min_vaddr)
			min_vaddr = ph[i].p_vaddr;
		elf_addr_t end = ph[i].p_vaddr + ph[i].p_memsz;
		if (end > max_vaddr)
			max_vaddr = end;
	}
	if (min_vaddr == UINT64_MAX) {
		FLUX_LOG(FLUX_LOG_ERR, "no PT_LOAD found in %s\n", path);
		err = -FLUX_ENOEXEC;
		goto out_munmap;
	}
	map_start = align_down(min_vaddr, PGSIZE_4KB);
	map_end = align_up(max_vaddr, PGSIZE_4KB);
	total_sz = map_end - map_start;

	if (eh->e_type == ET_DYN) {
		err = flux_reserve_et_dyn_region(map_start, total_sz, &rsvd);
		if (!err) {
			did_reserve = true;
		} else if (err == -FLUX_ENOSYS) {
			rsvd = NULL;
		} else {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to reserve ET_DYN region for %s\n",
				 path);
			goto out_munmap;
		}
	} else if (eh->e_type == ET_EXEC) {
		/*
		 * ET_EXEC binaries contain absolute virtual addresses in places
		 * like .init_array. They must be mapped at their linked VAs.
		 */
		rsvd = (void *)map_start;
#ifdef MAP_FIXED_NOREPLACE
		reserve_flags |= MAP_FIXED_NOREPLACE;
#else
		reserve_flags |= MAP_FIXED;
#endif
	} else {
		FLUX_LOG(FLUX_LOG_ERR, "unsupported ELF type %u in %s\n",
			 eh->e_type, path);
		err = -FLUX_ENOEXEC;
		goto out_munmap;
	}
	if (!did_reserve) {
		rsvd = flux_sys_mmap_checked(rsvd, total_sz, FLUX_PROT_NONE,
					     reserve_flags, -1, 0);
		if (rsvd == MAP_FAILED) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to mmap reserved region for %s\n",
				 path);
			err = -FLUX_ENOMEM;
			goto out_munmap;
		}
	}

	if (eh->e_type == ET_EXEC && rsvd != (void *)map_start) {
		FLUX_LOG(FLUX_LOG_ERR,
			 "failed to reserve ET_EXEC at linked VA %p (got %p)\n",
			 (void *)map_start, rsvd);
		err = -FLUX_ENOMEM;
		goto out_munmap1;
	}

	base = rsvd - map_start;

	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;

		/* map segment */
		elf_addr_t seg_start = align_down(ph[i].p_vaddr, PGSIZE_4KB);
		elf_addr_t seg_end =
			align_up(ph[i].p_vaddr + ph[i].p_memsz, PGSIZE_4KB);
		void *seg_addr = base + seg_start;
		size_t seg_len = seg_end - seg_start;
		elf_addr_t file_end =
			align_up(ph[i].p_vaddr + ph[i].p_filesz, PGSIZE_4KB);
		size_t file_map_len = ph[i].p_filesz ? file_end - seg_start : 0;
		size_t file_sz = 0;
		int seg_prot = flux_prot_from_pflags(ph[i].p_flags);
		void *bss_addr = base + seg_start + file_map_len;
		size_t bss_len = seg_len - file_map_len;

		if (ph[i].p_filesz > ph[i].p_memsz) {
			FLUX_LOG(FLUX_LOG_ERR, "segment %d file sz > mem sz\n",
				 i);
			err = -FLUX_ENOEXEC;
			goto out_munmap1;
		}

#ifdef CONFIG_FLUX_MPK
		if ((seg_prot & (FLUX_PROT_WRITE | FLUX_PROT_EXEC)) ==
		    (FLUX_PROT_WRITE | FLUX_PROT_EXEC)) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "refusing writable executable segment %d in %s\n",
				 i, path);
			err = -FLUX_ENOEXEC;
			goto out_munmap1;
		}
#endif

		if (file_map_len) {
			seg_addr = flux_sys_mmap_checked(
				seg_addr, file_map_len, PROT_READ | PROT_WRITE,
				MAP_FIXED | MAP_PRIVATE, fd,
				align_down(ph[i].p_offset, PGSIZE_4KB));
			if (seg_addr == MAP_FAILED ||
			    seg_addr != base + seg_start) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "failed to mmap segment %d\n", i);
				err = -FLUX_ENOMEM;
				goto out_munmap1;
			}
		}

		/* bss (anonymous) mapping beyond file-backed region */
		if (bss_len) {
			FLUX_LOG(FLUX_LOG_DEBUG,
				 "segment %d bss_addr=%p bss_len=%lx\n", i,
				 bss_addr, bss_len);
			if (flux_sys_mmap_checked(
				    bss_addr, bss_len, PROT_READ | PROT_WRITE,
				    MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1,
				    0) == MAP_FAILED) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "failed to mmap bss %d\n", i);
				err = -FLUX_ENOMEM;
				goto out_munmap1;
			}
		}

		/* bss zeros */
		file_sz = (ph[i].p_filesz + (ph[i].p_vaddr - seg_start));
		if (file_sz < file_map_len)
			memset(seg_addr + file_sz, 0, file_map_len - file_sz);

		if (scan_binary && (seg_prot & FLUX_PROT_EXEC)) {
			size_t bad_offset;

			err = flux_mpk_scan_binary(seg_addr, seg_len,
						   &bad_offset);
			if (err < 0) {
				FLUX_LOG(FLUX_LOG_ERR,
					 "unsafe MPK instruction in %s segment %d at +0x%zx\n",
					 path, i, bad_offset);
				goto out_munmap1;
			}
		}

		if (mprotect(seg_addr, seg_len, seg_prot) < 0) {
			FLUX_LOG(FLUX_LOG_ERR,
				 "failed to mprotect segment %d\n", i);
			err = -FLUX_ENOMEM;
			goto out_munmap1;
		}
	}

	elf_addr_t at_phdr = 0;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_PHDR)
			at_phdr = (elf_addr_t)base + ph[i].p_vaddr;
	}
	if (!at_phdr)
		at_phdr = (elf_addr_t)base + eh->e_phoff;

	info->addr = rsvd;
	info->sz = total_sz;
	info->base = base;
	info->entry = (elf_addr_t)base + eh->e_entry;
	info->phdr = at_phdr;
	info->phnum = eh->e_phnum;

	FLUX_LOG(FLUX_LOG_DEBUG, "loaded %s at base %p (entry %p)\n", path,
		 info->base, (void *)info->entry);

	/* don't touch rsvd */
	goto out_munmap;
out_munmap1:
	munmap(rsvd, total_sz);
out_munmap:
	munmap(addr, st.st_size);
out_close:
	flux_sys_close(fd);
out:
	return err;
}

static inline void flux_get_random_bytes(char *buf, size_t len)
{
	int fd;
	long ret;

	fd = flux_sys_open("/dev/urandom", FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open /dev/urandom\n");
		memset(buf, 0, len);
		return;
	}

	ret = flux_sys_read(fd, buf, len);
	if (ret != (long)len) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to read /dev/urandom\n");
		memset(buf, 0, len);
	}
	flux_sys_close(fd);
}

#define STACK_ALLOC(sp, len) (sp -= len)

static int flux_build_auxv(Elf64_auxv_t **out, size_t *outc,
			   const struct flux_elf_info *elf,
			   const struct flux_elf_info *elf_interp,
			   elf_addr_t *stack, const char *file)
{
	Elf64_auxv_t *vec, *ptr;
	elf_addr_t sp = *stack;
	char random_bytes[16];
	char *platform, *base_platform;
	void *auxv_random, *auxv_platform, *auxv_base_platform, *auxv_execfn;

#define AT_VECTOR_SIZE 64
	vec = malloc(AT_VECTOR_SIZE * sizeof(Elf64_auxv_t));
	if (!vec)
		return -FLUX_ENOMEM;
	memset(vec, 0, AT_VECTOR_SIZE * sizeof(Elf64_auxv_t));

	ptr = vec;
#define NEW_AUX_ENT(id, val)                                                   \
	do {                                                                   \
		*ptr++ =                                                       \
			(Elf64_auxv_t){ .a_type = (id), .a_un.a_val = (val) }; \
	} while (0)

	flux_get_random_bytes((char *)random_bytes, sizeof(random_bytes));
	auxv_random = (void *)STACK_ALLOC(sp, sizeof(random_bytes));
	memcpy(auxv_random, random_bytes, sizeof(random_bytes));

	platform = (char *)getauxval(AT_PLATFORM);
	if (platform) {
		size_t len = strlen(platform) + 1;
		auxv_platform = (void *)STACK_ALLOC(sp, len);
		memcpy(auxv_platform, platform, len);
	} else {
		auxv_platform = NULL;
	}

	base_platform = (char *)getauxval(AT_BASE_PLATFORM);
	if (base_platform) {
		size_t len = strlen(base_platform) + 1;
		auxv_base_platform = (void *)STACK_ALLOC(sp, len);
		memcpy(auxv_base_platform, base_platform, len);
	} else {
		auxv_base_platform = NULL;
	}

	auxv_execfn = NULL;
	if (file) {
		size_t len = strlen(file) + 1;
		auxv_execfn = (void *)STACK_ALLOC(sp, len);
		memcpy(auxv_execfn, file, len);
	}

	NEW_AUX_ENT(AT_HWCAP, getauxval(AT_HWCAP));
	NEW_AUX_ENT(AT_PAGESZ, PGSIZE_4KB);
	NEW_AUX_ENT(AT_CLKTCK, getauxval(AT_CLKTCK));
	NEW_AUX_ENT(AT_PHDR, elf->phdr);
	NEW_AUX_ENT(AT_PHENT, sizeof(Elf64_Phdr));
	NEW_AUX_ENT(AT_PHNUM, elf->phnum);
	NEW_AUX_ENT(AT_BASE, elf_interp ? (elf_addr_t)elf_interp->base : 0);
	NEW_AUX_ENT(AT_FLAGS, 0);
	NEW_AUX_ENT(AT_ENTRY, elf->entry);
	NEW_AUX_ENT(AT_UID, flux_sys_getuid());
	NEW_AUX_ENT(AT_EUID, flux_sys_geteuid());
	NEW_AUX_ENT(AT_GID, flux_sys_getgid());
	NEW_AUX_ENT(AT_EGID, flux_sys_getegid());
	NEW_AUX_ENT(AT_SECURE, 0);
	NEW_AUX_ENT(AT_RANDOM, (elf_addr_t)auxv_random);
	NEW_AUX_ENT(AT_HWCAP2, getauxval(AT_HWCAP2));
	NEW_AUX_ENT(AT_EXECFN, (elf_addr_t)auxv_execfn);
	NEW_AUX_ENT(AT_SYSINFO_EHDR, (elf_addr_t)flux_vdso_ehdr());
	if (auxv_platform)
		NEW_AUX_ENT(AT_PLATFORM, (elf_addr_t)auxv_platform);
	if (auxv_base_platform)
		NEW_AUX_ENT(AT_BASE_PLATFORM, (elf_addr_t)auxv_base_platform);
	NEW_AUX_ENT(AT_RSEQ_FEATURE_SIZE, getauxval(AT_RSEQ_FEATURE_SIZE));
	NEW_AUX_ENT(AT_RSEQ_ALIGN, getauxval(AT_RSEQ_ALIGN));
	NEW_AUX_ENT(AT_NULL, 0);

	*outc = ptr - vec;
	*out = vec;

	*stack = sp;

	return 0;
}

static void flux_build_stack(int argc, char **argv, char **envp,
			     Elf64_auxv_t *auxv, size_t auxc, elf_addr_t *stack)
{
	char **new_argv, **new_envp;
	size_t envc = 0;
	elf_addr_t sp = *stack;
	size_t items;
	int i;

	while (envp[envc])
		envc++;

	new_argv = alloca((argc + 1) * sizeof(char *));
	new_envp = alloca((envc + 1) * sizeof(char *));

	for (i = argc - 1; i >= 0; i--) {
		size_t len = strlen(argv[i]) + 1;
		memcpy((void *)STACK_ALLOC(sp, len), argv[i], len);
		new_argv[i] = (char *)sp;
	}
	new_argv[argc] = NULL;

	for (i = (int)envc - 1; i >= 0; i--) {
		size_t len = strlen(envp[i]) + 1;
		memcpy((void *)STACK_ALLOC(sp, len), envp[i], len);
		new_envp[i] = (char *)sp;
	}
	new_envp[envc] = NULL;

	/*
	 * Match Linux create_elf_tables(): reserve argc/argv/envp/auxv as an
	 * aligned block after placing the strings below the initial stack top.
	 */
	items = 1 + (size_t)(argc + 1) + (envc + 1) +
		(auxc * (sizeof(Elf64_auxv_t) / sizeof(elf_addr_t)));
	sp = (sp - items * sizeof(elf_addr_t)) & ~0xfUL;

	{
		elf_addr_t *spv = (elf_addr_t *)sp;

		*spv++ = (elf_addr_t)argc;

		for (i = 0; i < argc; i++)
			*spv++ = (elf_addr_t)new_argv[i];
		*spv++ = 0;

		for (i = 0; i < (int)envc; i++)
			*spv++ = (elf_addr_t)new_envp[i];
		*spv++ = 0;

		memcpy(spv, auxv, sizeof(Elf64_auxv_t) * auxc);
	}

	*stack = sp;
}

static void flux_free_elf(struct flux_elf_info *info)
{
	if (info->addr && info->sz)
		munmap(info->addr, info->sz);
	memset(info, 0, sizeof(*info));
}

static void flux_free_envp(char **envp)
{
	size_t i = 0;

	if (!envp)
		return;

	while (envp[i])
		free(envp[i++]);
	free(envp);
}

static size_t flux_count_envp(char **envp)
{
	size_t count = 0;

	if (!envp)
		return 0;

	while (envp[count])
		count++;

	return count;
}

static inline size_t flux_env_key_len(const char *env)
{
	const char *eq = strchr(env, '=');

	return eq ? (size_t)(eq - env) : strlen(env);
}

static bool flux_env_same_key(const char *lhs, const char *rhs)
{
	size_t lhs_len = flux_env_key_len(lhs);
	size_t rhs_len = flux_env_key_len(rhs);

	return lhs_len == rhs_len && strncmp(lhs, rhs, lhs_len) == 0;
}

static const char *flux_find_env_entry(char **envp, int env_num,
				       const char *key)
{
	size_t key_len = strlen(key);
	int i;

	if (!envp)
		return NULL;

	for (i = 0; i < env_num; i++) {
		if (!envp[i])
			continue;
		if (strncmp(envp[i], key, key_len) == 0 &&
		    envp[i][key_len] == '=')
			return envp[i];
	}

	return NULL;
}

static int flux_env_set_or_append(char **envp, size_t *envc, size_t env_cap,
				  const char *entry)
{
	char *copy;
	size_t i;

	copy = strdup(entry);
	if (!copy)
		return -1;

	for (i = 0; i < *envc; i++) {
		if (!flux_env_same_key(envp[i], entry))
			continue;

		free(envp[i]);
		envp[i] = copy;
		return 0;
	}

	if (*envc >= env_cap) {
		free(copy);
		return -1;
	}

	envp[*envc] = copy;
	(*envc)++;
	envp[*envc] = NULL;

	return 0;
}

static void flux_build_envp(char **base_envp, char ***out)
{
	const struct flux_oci_cfg *oci = flux_oci_cfg_get();
	char **new_envp = NULL;
	size_t base_envc = 0;
	size_t oci_envc = 0;
	size_t cfg_envc = 0;
	size_t extra_envc = 0;
	size_t compat_envc = 0;
	size_t env_cap;
	size_t envc = 0;
	size_t i;

	base_envc = flux_count_envp(base_envp);
	if (oci && oci->env && oci->env_num > 0)
		oci_envc = (size_t)oci->env_num;
	if (run_cfg && run_cfg->env && run_cfg->env_num > 0)
		cfg_envc = (size_t)run_cfg->env_num;

	if (flux_extra_envp) {
		for (extra_envc = 0; flux_extra_envp[extra_envc]; extra_envc++)
			;
	}

	if ((base_envp || oci) && run_cfg && run_cfg->ld_path &&
	    run_cfg->ld_path[0] &&
	    flux_find_env_entry(run_cfg->env, run_cfg->env_num,
				"LD_LIBRARY_PATH") &&
	    flux_find_env_entry(base_envp ? base_envp : oci->env,
				base_envp ? (int)base_envc : oci->env_num,
				"LD_LIBRARY_PATH") == NULL)
		compat_envc = 1;

	if (base_envp)
		env_cap = base_envc + compat_envc + extra_envc;
	else if (oci)
		env_cap = oci_envc + compat_envc + extra_envc;
	else
		env_cap = cfg_envc + extra_envc;
	new_envp = calloc(env_cap + 1, sizeof(char *));
	if (!new_envp)
		goto out;

	if (base_envp) {
		for (i = 0; base_envp[i]; i++) {
			if (flux_env_set_or_append(new_envp, &envc, env_cap,
						   base_envp[i]) < 0)
				goto out_free;
		}
	}

	if (!base_envp && oci && oci->env && oci->env_num > 0) {
		for (i = 0; i < (size_t)oci->env_num; i++) {
			if (flux_env_set_or_append(new_envp, &envc, env_cap,
						   oci->env[i]) < 0)
				goto out_free;
		}
	}

	if ((base_envp || oci) && run_cfg && run_cfg->ld_path &&
	    run_cfg->ld_path[0]) {
		const char *ld_library_path = flux_find_env_entry(
			run_cfg->env, run_cfg->env_num, "LD_LIBRARY_PATH");

		if (ld_library_path &&
		    flux_find_env_entry(new_envp, (int)envc,
					"LD_LIBRARY_PATH") == NULL) {
			if (flux_env_set_or_append(new_envp, &envc, env_cap,
						   ld_library_path) < 0)
				goto out_free;
		}
	}

	if (!base_envp && !oci && run_cfg && run_cfg->env &&
	    run_cfg->env_num > 0) {
		for (i = 0; i < (size_t)run_cfg->env_num; i++) {
			if (flux_env_set_or_append(new_envp, &envc, env_cap,
						   run_cfg->env[i]) < 0)
				goto out_free;
		}
	}

	if (flux_extra_envp) {
		for (i = 0; flux_extra_envp[i]; i++) {
			if (flux_env_set_or_append(new_envp, &envc, env_cap,
						   flux_extra_envp[i]) < 0)
				goto out_free;
		}
	}

	*out = new_envp;

	return;
out_free:
	flux_free_envp(new_envp);
out:
	*out = NULL;
	return;
}

int flux_elf_main(const char *filename, int argc, char **argv, char **envp,
		  void (*fn)(void *entry, unsigned long stack))
{
	const char *file;
	const char *exec_file;
	char *interp = NULL;
	char *script_interp = NULL;
	char *script_interp_arg = NULL;
	char **script_argv = NULL;
	char **exec_argv = argv;
	int exec_argc = argc;
	struct flux_elf_info elf = { 0 }, elf_interp = { 0 };
	Elf64_auxv_t *auxv = NULL;
	size_t auxc = 0;
	char **stack_envp = NULL;
	elf_addr_t entry, stack;
	int err = -FLUX_ENOMEM;

	if (!argc)
		return -FLUX_EINVAL;

	file = (filename && filename[0]) ? filename : argv[0];
	exec_file = file;

	err = flux_parse_script_shebang(file, &script_interp,
					&script_interp_arg);
	if (err == 0) {
		err = flux_build_script_argv(argc, argv, exec_file,
					     script_interp, script_interp_arg,
					     &exec_argc, &script_argv);
		if (err < 0)
			goto out;

		exec_argv = script_argv;
		file = script_interp;

		FLUX_LOG(FLUX_LOG_DEBUG, "script file: %s\n", exec_file);
		FLUX_LOG(FLUX_LOG_DEBUG, "script interp: %s%s%s\n",
			 script_interp, script_interp_arg ? " " : "",
			 script_interp_arg ? script_interp_arg : "");
	} else if (err != -FLUX_ENOEXEC) {
		goto out;
	}

	flux_set_proc_self_exe(file);

	if ((err = flux_find_interp(file, &interp)) < 0)
		goto out;

	FLUX_LOG(FLUX_LOG_DEBUG, "elf file: %s\n", file);
	FLUX_LOG(FLUX_LOG_DEBUG, "elf interp: %s\n", interp ? interp : "NULL");

	if ((err = __load_elf(file, &elf, true)) < 0)
		goto out_free_interp_path;

	entry = elf.entry;
	if (interp) {
		/* The Flux glibc interpreter is the trusted syscall shim. */
		if ((err = __load_elf(interp, &elf_interp, false)) < 0) {
			flux_free_elf(&elf);
			goto out_free_elf;
		}
		entry = elf_interp.entry;
	}

	stack = (elf_addr_t)flux_sys_mmap_checked(
		NULL, FLUX_USER_STACK_SIZE, FLUX_PROT_READ | FLUX_PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == (elf_addr_t)MAP_FAILED)
		goto out_free_interp;

	stack += FLUX_USER_STACK_SIZE - PGSIZE_4KB;

	/* redzone */
	stack -= 128;

	if ((err = flux_build_auxv(&auxv, &auxc, &elf,
				   interp ? &elf_interp : NULL, &stack,
				   exec_file)) < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to build auxv\n");
		goto out_free_stack;
	}

	flux_build_envp(envp, &stack_envp);
	if (!stack_envp) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to build envp\n");
		err = -FLUX_ENOMEM;
		goto out_free_stack;
	}

	flux_build_stack(exec_argc, exec_argv, stack_envp, auxv, auxc, &stack);

	flux_free_envp(stack_envp);

	free(auxv);

	fn((void *)entry, stack);

	__builtin_unreachable();
out_free_stack:
	munmap((void *)stack, FLUX_USER_STACK_SIZE);
out_free_interp:
	flux_free_elf(&elf_interp);
out_free_elf:
	flux_free_elf(&elf);
out_free_interp_path:
	free(interp);
	flux_free_argv_copy(script_argv);
	free(script_interp_arg);
	free(script_interp);
out:
	return err;
}

int flux_load_elf(const char *filename, char **argv, char **envp,
		  flux_post_exec_fn_t fn)
{
	int argc = 0;

	while (argv[argc])
		argc++;

	return flux_elf_main(filename, argc, argv, envp, fn);
}
