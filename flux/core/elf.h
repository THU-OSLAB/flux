#ifndef _FLUX_ELF_H
#define _FLUX_ELF_H

#include <stddef.h>
#include <stdint.h>

#define FLUX_USER_STACK_SIZE (8 * 1024 * 1024)

typedef uint64_t elf_addr_t;

struct flux_elf_info {
	void *addr;
	size_t sz;
	void *base;
	elf_addr_t entry;
	elf_addr_t phdr;
	elf_addr_t phnum;
};

#endif
