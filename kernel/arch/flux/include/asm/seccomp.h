/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_SECCOMP_H
#define _ASM_FLUX_SECCOMP_H

#include <asm/unistd.h>
#include <uapi/linux/audit.h>

#define SECCOMP_ARCH_NATIVE		AUDIT_ARCH_X86_64
#define SECCOMP_ARCH_NATIVE_NR		NR_syscalls
#define SECCOMP_ARCH_NATIVE_NAME	"x86_64"

#include <asm-generic/seccomp.h>

#endif /* _ASM_FLUX_SECCOMP_H */
