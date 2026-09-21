#ifndef _FLUX_SIGNAL_ABI_H
#define _FLUX_SIGNAL_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#define FLUX_ABI_U64 __u64
#define FLUX_ABI_S32 __s32
#define FLUX_ABI_U32 __u32
#else
#include <stdint.h>
#define FLUX_ABI_U64 uint64_t
#define FLUX_ABI_S32 int32_t
#define FLUX_ABI_U32 uint32_t
#endif

#define FLUX_SIGNAL_ENTRY_COOKIE_TAG 0x465855494e540000ULL
#define FLUX_SIGNAL_ENTRY_COOKIE_TAG_MASK 0xffffffffffff0000ULL
#define FLUX_SIGNAL_ENTRY_COOKIE_VALID (1ULL << 15)
#define FLUX_SIGNAL_ENTRY_COOKIE_UIF (1ULL << 14)
#define FLUX_SIGNAL_ENTRY_COOKIE_CPU_MASK ((1ULL << 14) - 1)

#define FLUX_SIGNAL_STACK_SLOTS 64

struct flux_uintr_setup {
	FLUX_ABI_U64 handler;
	FLUX_ABI_U64 host_fsbase;
	FLUX_ABI_U64 signal_stack;
	FLUX_ABI_U64 signal_stack_slot_size;
	FLUX_ABI_S32 logical_cpu;
	FLUX_ABI_U32 reserved;
	FLUX_ABI_U64 synthetic_handler;
};

_Static_assert(sizeof(struct flux_uintr_setup) == 48, "flux_uintr_setup size");
_Static_assert(__builtin_offsetof(struct flux_uintr_setup, handler) == 0, "flux_uintr_setup handler offset");
_Static_assert(__builtin_offsetof(struct flux_uintr_setup, host_fsbase) == 8, "flux_uintr_setup host_fsbase offset");
_Static_assert(__builtin_offsetof(struct flux_uintr_setup, signal_stack) == 16, "flux_uintr_setup signal_stack offset");
_Static_assert(__builtin_offsetof(struct flux_uintr_setup, signal_stack_slot_size) == 24, "flux_uintr_setup signal_stack_slot_size offset");
_Static_assert(__builtin_offsetof(struct flux_uintr_setup, logical_cpu) == 32, "flux_uintr_setup logical_cpu offset");
_Static_assert(__builtin_offsetof(struct flux_uintr_setup, reserved) == 36, "flux_uintr_setup reserved offset");

_Static_assert(__builtin_offsetof(struct flux_uintr_setup, synthetic_handler) == 40, "flux_uintr_setup synthetic_handler offset");

#undef FLUX_ABI_U64
#undef FLUX_ABI_S32
#undef FLUX_ABI_U32

#endif /* _FLUX_SIGNAL_ABI_H */
