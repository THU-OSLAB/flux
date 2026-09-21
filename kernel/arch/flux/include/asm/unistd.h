/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_UNISTD_H
#define _ASM_FLUX_UNISTD_H

#include <uapi/asm/unistd.h>

#ifdef CONFIG_X86_64

#include <asm/unistd_64.h>

#define NR_syscalls (__NR_syscalls)

#define __ARCH_WANT_SYS_TIME
#define __ARCH_WANT_SYS_UTIME
#define __ARCH_WANT_SYS_FORK
#define __ARCH_WANT_SYS_VFORK
#define __ARCH_WANT_SYS_CLONE
#define __ARCH_WANT_SYS_CLONE3
#define __ARCH_WANT_SYS_WAITPID
#define __ARCH_WANT_NEW_STAT
#define __ARCH_WANT_SET_GET_RLIMIT
#define __ARCH_WANT_TIME32_SYSCALLS
#define __ARCH_WANT_SYS_ALARM
#define __ARCH_WANT_SYS_PAUSE
#define __ARCH_WANT_SYS_SIGNAL
#define __ARCH_WANT_SYS_SIGPENDING
#define __ARCH_WANT_SYS_SIGPROCMASK
#define __ARCH_WANT_SYS_GETPGRP


#else
#error "Flux only supports x86_64"
#endif

#define __SC_ASCII(t, a) #t "," #a

#define __ASCII_MAP0(m, ...)
#define __ASCII_MAP1(m, t, a) m(t, a)
#define __ASCII_MAP2(m, t, a, ...) m(t, a) "," __ASCII_MAP1(m, __VA_ARGS__)
#define __ASCII_MAP3(m, t, a, ...) m(t, a) "," __ASCII_MAP2(m, __VA_ARGS__)
#define __ASCII_MAP4(m, t, a, ...) m(t, a) "," __ASCII_MAP3(m, __VA_ARGS__)
#define __ASCII_MAP5(m, t, a, ...) m(t, a) "," __ASCII_MAP4(m, __VA_ARGS__)
#define __ASCII_MAP6(m, t, a, ...) m(t, a) "," __ASCII_MAP5(m, __VA_ARGS__)
#define __ASCII_MAP(n, ...) __ASCII_MAP##n(__VA_ARGS__)

#ifdef __MINGW32__
#define SECTION_ATTRS "n0"
#else
#define SECTION_ATTRS "a"
#endif

#define __SYSCALL_DEFINE_ARCH(x, name, ...)                              \
	asm(".section .syscall_defs,\"" SECTION_ATTRS "\"\n"             \
	    ".ascii \"#ifdef __NR" #name "\\n\"\n"                 \
	    ".ascii \"SYSCALL_DEFINE" #x "(" #name "," __ASCII_MAP(      \
		    x, __SC_ASCII, __VA_ARGS__) ")\\n\"\n"               \
						".ascii \"#endif\\n\"\n" \
						".section .text\n");
#endif
