#ifndef _UAPI_ASM_FLUX_REWRITE_H
#define _UAPI_ASM_FLUX_REWRITE_H

/* Marks the synthetic return address pushed by a rewritten two-byte syscall. */
#define FLUX_REWRITTEN_SYSCALL_TAG (1ULL << 63)

#endif /* _UAPI_ASM_FLUX_REWRITE_H */
