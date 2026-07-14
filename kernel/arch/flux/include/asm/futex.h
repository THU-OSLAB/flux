#ifndef _FLUX_ASM_FUTEX_H
#define _FLUX_ASM_FUTEX_H

#ifdef CONFIG_X86_64
#include <asm/x86/futex.h>
#else
#error "Unsupported architecture for futex implemtations"
#endif

#endif /* _FLUX_ASM_FUTEX_H */