#ifndef _ASM_FLUX_SWITCH_TO_H
#define _ASM_FLUX_SWITCH_TO_H

#ifdef CONFIG_X86_64
#include <asm/x86/switch_to.h>
#else
#error "Only x86_64 is supported"
#endif

#endif /* _ASM_FLUX_SWITCH_TO_H */