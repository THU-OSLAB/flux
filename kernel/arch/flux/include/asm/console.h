/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_CONSOLE_H
#define _ASM_FLUX_CONSOLE_H

#include <linux/types.h>
#include <uapi/asm/flux_oci.h>

int flux_cons_install_boot_stdio(void);
int flux_cons_install_session(u32 session_id);

#endif /* _ASM_FLUX_CONSOLE_H */
