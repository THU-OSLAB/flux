#ifndef _ASM_FLUX_ENTRY_COMMON_H
#define _ASM_FLUX_ENTRY_COMMON_H

/* required by linux/entry-common.h */

#ifdef CONFIG_X86_64
#include <asm/x86/entry-common.h>
#else
#error "Only x86_64 is supported in Flux"
#endif

#endif /* _ASM_FLUX_ENTRY_COMMON_H */