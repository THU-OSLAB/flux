// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal x86-64 module relocation support for Flux. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/elf.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleloader.h>
#include <linux/types.h>

#define R_X86_64_NONE		0
#define R_X86_64_64		1
#define R_X86_64_PC32		2
#define R_X86_64_32		10
#define R_X86_64_32S		11
#define R_X86_64_PC64		24
#define R_X86_64_PLT32		4

int apply_relocate_add(Elf_Shdr *sechdrs,
		       const char *strtab,
		       unsigned int symindex,
		       unsigned int relsec,
		       struct module *me)
{
	Elf_Rela *rel = (void *)sechdrs[relsec].sh_addr;
	unsigned int i;

	for (i = 0; i < sechdrs[relsec].sh_size / sizeof(*rel); i++) {
		Elf_Sym *sym = (Elf_Sym *)sechdrs[symindex].sh_addr +
			ELF_R_SYM(rel[i].r_info);
		void *loc = (void *)sechdrs[sechdrs[relsec].sh_info].sh_addr +
			rel[i].r_offset;
		u64 val = sym->st_value + rel[i].r_addend;

		switch (ELF_R_TYPE(rel[i].r_info)) {
		case R_X86_64_NONE:
			break;
		case R_X86_64_64:
			*(u64 *)loc = val;
			break;
		case R_X86_64_32:
			if (val != (u32)val)
				goto overflow;
			*(u32 *)loc = val;
			break;
		case R_X86_64_32S:
			if ((s64)val != (s32)val)
				goto overflow;
			*(s32 *)loc = val;
			break;
		case R_X86_64_PC32:
		case R_X86_64_PLT32:
			val -= (u64)loc;
			if ((s64)val != (s32)val)
				goto overflow;
			*(s32 *)loc = val;
			break;
		case R_X86_64_PC64:
			*(u64 *)loc = val - (u64)loc;
			break;
		default:
			pr_err("%s: unknown x86-64 rela relocation: %llu\n",
			       module_name(me), ELF_R_TYPE(rel[i].r_info));
			return -ENOEXEC;
		}
	}

	return 0;

overflow:
	pr_err("%s: overflow in x86-64 relocation type %llu\n",
	       module_name(me), ELF_R_TYPE(rel[i].r_info));
	return -ENOEXEC;
}
