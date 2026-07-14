/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/vmlinux.lds.h>
#include <asm/thread_info.h>
#include <asm/page.h>
#include <asm/cache.h>
#include <linux/export.h>

#define RO_EXCEPTION_TABLE_ALIGN	16

OUTPUT_FORMAT(CONFIG_OUTPUT_FORMAT)

jiffies = jiffies_64;

/* FIXME: symbols defined here outside individual sections don't have a final
 * address assigned because vmlinux is linked incrementally. In the final
 * linking step (when linked against an application) they can be rearranged away
 * from the other adjacent symbols or sections in this file which makes them
 * bogus.
 *
 * Fortunatelly, in most cases their are used for informational purposes only,
 * e.g. printing the init/code/data size early during boot.
 */
PHDRS {
	text PT_LOAD FLAGS(5);          /* R_E */
	data PT_LOAD FLAGS(6);          /* RW_ */
#ifdef CONFIG_X86_64
#ifdef CONFIG_SMP
	percpu PT_LOAD FLAGS(6);        /* RW_ */
#endif
	init PT_LOAD FLAGS(7);          /* RWE */
#endif
	note PT_NOTE FLAGS(0);          /* ___ */
}

SECTIONS
{
	/*
	 * Keep all text symbols in the same section, including init, to keep
	 * kallsyms partially working - at least for functions. This
	 * allows us to get non-bogus stack traces.
	 */
	/* Text and read-only data */
	.text : AT(ADDR(.text) - LOAD_OFFSET) {
		_text = . ;
		_stext = .;
		text = . ;

		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT

		_etext = .;
	} :text
	. = ALIGN(PAGE_SIZE);

	RO_DATA(PAGE_SIZE)
	. = ALIGN(PAGE_SIZE);

	/* Data */
	.data : AT(ADDR(.data) - LOAD_OFFSET) {
		/* Start of data section */
		_sdata = .;

		/* init_task */
		INIT_TASK_DATA(THREAD_SIZE)

		PAGE_ALIGNED_DATA(PAGE_SIZE)

		CACHELINE_ALIGNED_DATA(L1_CACHE_BYTES)

		DATA_DATA
		CONSTRUCTORS

		/* rarely changed data like cpu maps */
		READ_MOSTLY_DATA(L1_CACHE_BYTES)

		/* End of data section */
		_edata = .;
	} :data

	/* Init code and data - will be freed after init */
	. = ALIGN(PAGE_SIZE);
	.init.begin : AT(ADDR(.init.begin) - LOAD_OFFSET) {
		__init_begin = .; /* paired with __init_end */
	}

#if defined(CONFIG_X86_64) && defined(CONFIG_SMP)
	/*
	 * percpu offsets are zero-based on SMP.  PERCPU_VADDR() changes the
	 * output PHDR, so the next output section - .init.text - should
	 * start another segment - init.
	 */
	. = ALIGN(PAGE_SIZE);
	PERCPU_VADDR(L1_CACHE_BYTES, , :percpu)
#endif

	INIT_TEXT_SECTION(PAGE_SIZE)
#ifdef CONFIG_X86_64
	:init
#endif

	INIT_DATA_SECTION(16)

	. = ALIGN(PAGE_SIZE);

	/* freed after init ends here */
	.init.end : AT(ADDR(.init.end) - LOAD_OFFSET) {
		__init_end = .;
	}

	BSS_SECTION(0, 0, 0)

	. = ALIGN(PAGE_SIZE);
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG

	DISCARDS
}

