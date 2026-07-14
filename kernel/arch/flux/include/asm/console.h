/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_CONSOLE_H
#define _ASM_FLUX_CONSOLE_H

#include <linux/types.h>
#include <uapi/asm/flux_oci.h>

struct task_struct;

enum flux_cons_kind {
	FLUX_CONS_KIND_NONE = 0,
	FLUX_CONS_KIND_STDIO = 1,
	FLUX_CONS_KIND_TTY = 2,
};

struct flux_cons {
	u32 id;
	u32 kind;
};

int flux_cons_install_boot_stdio(void);
const struct flux_cons *flux_cons_lookup(u32 session_id);
int flux_cons_install_stdio(const struct flux_cons *cons);
int flux_cons_install_tty(const struct flux_cons *cons);

#endif /* _ASM_FLUX_CONSOLE_H */
