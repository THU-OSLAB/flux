/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FLUX_MPK_UACCESS_H
#define _ASM_FLUX_MPK_UACCESS_H

#include <linux/types.h>

#ifdef CONFIG_FLUX_MPK
int flux_mpk_uaccess_init(void);
void flux_mpk_uaccess_enter(void);
void flux_mpk_uaccess_exit(void);
unsigned int flux_mpk_uaccess_suspend(void);
void flux_mpk_uaccess_resume(unsigned int depth);
void flux_mpk_uaccess_reset(void);
bool flux_mpk_uaccess_ok(const void __user *addr, unsigned long len);
bool flux_mpk_uaccess_range_ok(const void __user *addr, unsigned long len);
#else
static inline int flux_mpk_uaccess_init(void)
{
	return 0;
}

static inline bool flux_mpk_uaccess_ok(const void __user *addr,
				       unsigned long len)
{
	return true;
}

static inline void flux_mpk_uaccess_enter(void)
{
}

static inline void flux_mpk_uaccess_exit(void)
{
}

static inline unsigned int flux_mpk_uaccess_suspend(void)
{
	return 0;
}

static inline void flux_mpk_uaccess_resume(unsigned int depth)
{
}

static inline void flux_mpk_uaccess_reset(void)
{
}

static inline bool flux_mpk_uaccess_range_ok(const void __user *addr,
					     unsigned long len)
{
	return true;
}
#endif

#endif /* _ASM_FLUX_MPK_UACCESS_H */
