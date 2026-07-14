#ifndef _ASM_FLUX_LINKAGE_H
#define _ASM_FLUX_LINKAGE_H

#define __ALIGN .balign CONFIG_FUNCTION_ALIGNMENT, 0x90;
#define __ALIGN_STR __stringify(__ALIGN)

#if defined(CONFIG_CALL_PADDING) && !defined(__DISABLE_EXPORTS) && \
	!defined(BUILD_VDSO)
#define FUNCTION_PADDING .skip CONFIG_FUNCTION_ALIGNMENT, 0x90;
#else
#define FUNCTION_PADDING
#endif

#if (CONFIG_FUNCTION_ALIGNMENT > 8) && !defined(__DISABLE_EXPORTS) && \
	!defined(BUILD_VDSO)
#define __FUNC_ALIGN \
	__ALIGN;     \
	FUNCTION_PADDING
#else
#define __FUNC_ALIGN __ALIGN
#endif

#define ASM_FUNC_ALIGN __stringify(__FUNC_ALIGN)
#define SYM_F_ALIGN __FUNC_ALIGN



#endif