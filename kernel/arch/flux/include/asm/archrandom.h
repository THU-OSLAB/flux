#ifndef _ASM_FLUX_ARCHRANDOM_H
#define _ASM_FLUX_ARCHRANDOM_H

#ifdef CONFIG_X86_64
#include <asm/x86/archrandom.h>
#else
#include <asm-generic/archrandom.h>
#endif

#endif /* _ASM_FLUX_ARCHRANDOM_H */