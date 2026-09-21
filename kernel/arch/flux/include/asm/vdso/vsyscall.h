/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_FLUX_VDSO_VSYSCALL_H
#define __ASM_FLUX_VDSO_VSYSCALL_H

#include <asm/vdso.h>

static __always_inline struct vdso_data *__arch_get_k_vdso_data(void)
{
	return flux_vdso_data;
}
#define __arch_get_k_vdso_data __arch_get_k_vdso_data

#include <asm-generic/vdso/vsyscall.h>
#endif
