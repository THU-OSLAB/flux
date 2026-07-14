#ifndef _FLUX_KMOD_MPK_H
#define _FLUX_KMOD_MPK_H

#include <linux/types.h>

#define FLUX_MPK_KERNEL_PKEY 0
#define FLUX_MPK_APP_PKEY 1
#define FLUX_MPK_SHARED_PKEY 2
#define FLUX_MPK_PROTECTED_MASK (3U << (2 * FLUX_MPK_KERNEL_PKEY))
#define FLUX_MPK_KERNEL_PKRU 0U
#define FLUX_MPK_APP_PKRU 0xffffffe3U

struct flux_mm_ctx;

int flux_mpk_enable(struct flux_mm_ctx *ctx);
void flux_mpk_ctx_release(struct flux_mm_ctx *ctx);
int flux_mpk_validate_app_range(struct flux_mm_ctx *ctx, unsigned long arg);
int flux_mpk_hook_init(void);
void flux_mpk_hook_exit(void);

#endif /* _FLUX_KMOD_MPK_H */
