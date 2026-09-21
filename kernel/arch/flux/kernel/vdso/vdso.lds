/* SPDX-License-Identifier: GPL-2.0 */

/*
 * glibc's vDSO setup expects the ELF image to have one PT_LOAD beginning at
 * virtual address zero. Keep metadata and text in a single read-only,
 * executable segment, matching the Linux x86 vDSO layout.
 */
SECTIONS
{
	/* Namespace vvar, then the system-wide timekeeping page. */
	PROVIDE_HIDDEN(_vdso_data = . - 8192);
	PROVIDE_HIDDEN(_timens_data = . - 4096);
	. = SIZEOF_HEADERS;

	.hash : { *(.hash) } :text
	.gnu.hash : { *(.gnu.hash) } :text
	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text
	.gnu.version : { *(.gnu.version) } :text
	.gnu.version_d : { *(.gnu.version_d) } :text
	.gnu.version_r : { *(.gnu.version_r) } :text
	.dynamic : { *(.dynamic) } :text :dynamic

	.rodata : {
		*(.rodata*)
		*(.data*)
		*(.sdata*)
		*(.got.plt)
		*(.got)
		*(.bss*)
		*(.dynbss*)
	} :text

	.text : { *(.text*) } :text =0x90909090

	/DISCARD/ : {
		*(.eh_frame*)
		*(.note.gnu.property)
	}
}

PHDRS
{
	text PT_LOAD FLAGS(5) FILEHDR PHDRS; /* PF_R | PF_X */
	dynamic PT_DYNAMIC FLAGS(4);         /* PF_R */
}
