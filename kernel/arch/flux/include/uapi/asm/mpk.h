/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_FLUX_MPK_H
#define _UAPI_ASM_FLUX_MPK_H

#define FLUX_MPK_KERNEL_PKEY 0
#define FLUX_MPK_APP_PKEY 1
#define FLUX_MPK_SHARED_PKEY 2

#define FLUX_MPK_KERNEL_PKRU 0

/*
 * key 0: no access, key 1: read/write, key 2: read-only.
 * Unused keys are disabled as well.
 */
#define FLUX_MPK_APP_PKRU 0xffffffe3

#define FLUX_MPK_FAULT_CMP64 2

#if !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)
/* Copied from a residual record; contains no pointer into record storage. */
struct flux_mpk_cmp64 {
	unsigned long address;
	unsigned long lhs;
	unsigned int length;
};
#endif

/*
 * Per-CPU UIRET and ordinary syscall-return frames live in separate pages of
 * one shared-pkey area. The application PKRU can read the area but cannot
 * write it; the kernel PKRU populates each 64-byte slot.
 *
 * Ordinary returns also need POPFQ without writing the application stack.
 * Each CPU gets a page pair after the slots: an application-writable UINTR
 * landing page followed by a shared-pkey page containing the flags word.
 * RSP points at the first word of the latter page. Hardware UINTR delivery
 * and entry scratch then stay entirely in the preceding writable page,
 * while POPFQ reads an application-read-only word. The final MOV/JMP return
 * preserves UIF and never writes below the application's saved RSP.
 */
#define FLUX_MPK_RETURN_ADDR 0x101000
#define FLUX_MPK_RETURN_PAGE_SIZE 4096
#define FLUX_MPK_RETURN_SLOT_SIZE 64
#define FLUX_MPK_RETURN_SLOT_CAPACITY \
	(FLUX_MPK_RETURN_PAGE_SIZE / FLUX_MPK_RETURN_SLOT_SIZE)
#define FLUX_SYSCALL_RETURN_ADDR \
	(FLUX_MPK_RETURN_ADDR + FLUX_MPK_RETURN_PAGE_SIZE)

#define FLUX_SYSCALL_FLAGS_STACK_SHIFT 13
#define FLUX_SYSCALL_FLAGS_STACK_SIZE (1 << FLUX_SYSCALL_FLAGS_STACK_SHIFT)
#define FLUX_SYSCALL_FLAGS_STACK_ADDR \
	(FLUX_SYSCALL_RETURN_ADDR + FLUX_MPK_RETURN_PAGE_SIZE)
#define FLUX_SYSCALL_FLAGS_ADDR \
	(FLUX_SYSCALL_FLAGS_STACK_ADDR + FLUX_MPK_RETURN_PAGE_SIZE)
#define FLUX_MPK_RETURN_AREA_SIZE \
	(2 * FLUX_MPK_RETURN_PAGE_SIZE + \
	 FLUX_MPK_RETURN_SLOT_CAPACITY * FLUX_SYSCALL_FLAGS_STACK_SIZE)

#endif /* _UAPI_ASM_FLUX_MPK_H */
