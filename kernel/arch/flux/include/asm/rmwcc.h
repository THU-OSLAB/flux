#ifndef _ASM_FLUX_RMWcc
#define _ASM_FLUX_RMWcc

#ifdef CONFIG_X86_64
#include <asm/x86/rmwcc.h>
#else
#error "Unsupported architecture for rmwcc.h"
#endif

#endif