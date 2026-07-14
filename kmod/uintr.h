#ifndef _FLUX_UINTR_H
#define _FLUX_UINTR_H

#define X86_FEATURE_UINTR (18 * 32 + 5) /* User Interrupts support */
#define DISABLE_UINTR (1 << (X86_FEATURE_UINTR & 31))

/* User Interrupt interface */
#define MSR_IA32_UINTR_RR 0x985
#define MSR_IA32_UINTR_HANDLER 0x986
#define MSR_IA32_UINTR_STACKADJUST 0x987
#define MSR_IA32_UINTR_MISC 0x988 /* 39:32-UINV, 31:0-UITTSZ */
#define MSR_IA32_UINTR_PD 0x989
#define MSR_IA32_UINTR_TT 0x98a

#define X86_CR4_UINTR_BIT 25 /* enable User Interrupts support */
#define X86_CR4_UINTR _BITUL(X86_CR4_UINTR_BIT)

#define UINTR_UPID_STATUS_ON 0x0 /* Outstanding notification */
#define UINTR_UPID_STATUS_SN 0x1 /* Suppressed notification */

#define UINTR_UITT_VALID_BIT 0x0

/*
 * State component 14 is supervisor state used for User Interrupts state.
 * The size of this state is 48 bytes
 */
struct uintr_state {
	__u64 handler;
	__u64 stack_adjust;
	struct {
		__u32 uitt_size;
		__u8 uinv;
		__u8 pad1;
		__u8 pad2;
		__u8 pad3 : 7;
		__u8 uif : 1;
	} __packed misc;
	__u64 upid_addr;
	__u64 uirr;
	__u64 uitt_addr;
} __packed;

/* User Interrupt Target Table Entry (UITTE) */
struct uintr_uitt_entry {
	__u8 valid; /* bit 0: valid, bit 1-7: reserved */
	__u8 user_vec;
	__u8 reserved[6];
	__u64 target_upid_addr;
} __packed __aligned(16);

struct uintr_upid {
	union {
		struct {
			__u8 status; /* bit 0: ON, bit 1: SN, bit 2-7: reserved */
			__u8 reserved1; /* Reserved */
			__u8 nv; /* Notification vector */
			__u8 reserved2; /* Reserved */
			__u32 ndst; /* Notification destination */
		} nc __packed; /* Notification control */
		long unsigned int word_val;
	};
	__u64 puir; /* Posted user interrupt requests */
} __aligned(64);

#define XFEATURE_UINTR 14
#define XFEATURE_MASK_UINTR (1 << XFEATURE_UINTR)

#include <asm/apic.h>
#include <asm/local.h>
#include <asm/irq_vectors.h>

/* Use KVM's posted interrupt vector */
#define UIPI_APIC_VECTOR POSTED_INTR_WAKEUP_VECTOR

#define MAX_NR_USER_VEC 16
#define UINTR_MAX_SIGNAL_NEST 64

struct uintr_ctx {
	unsigned long handler;
	struct kref refcount;
	bool is_admin;

	/* sender UITT table */
	struct uintr_uitt_entry *uitt[MAX_NR_USER_VEC];
};

struct uintr_xstate {
	struct xregs_state xregs;
	struct uintr_state uintr;
} __packed __aligned(64);

struct uintr_percpu {
	struct task_struct *assigned_task;
	struct uintr_ctx *assigned_ctx;

	bool state_loaded;
	bool is_admin_ctx;
	struct uintr_xstate cur_xstate;
	
	long syscall_nr;
	u8 sig_uif_stack[UINTR_MAX_SIGNAL_NEST];
	u8 sig_uif_depth;
};

extern void uintr_cleanup_core(struct uintr_percpu *p, int cpu);
extern void uintr_assign_core(struct uintr_ctx *ctx, u64 stack);

extern void uintr_deliver_ipi(struct uintr_percpu *p);

extern int uintr_init(void);
extern void uintr_exit(void);
extern long uintr_setup_percpu(struct file *filp, unsigned long handler);
extern void uintr_file_release(struct file *filp);

extern bool uintr_enabled;

static inline struct uintr_ctx *to_uintr_ctx(struct file *filp)
{
	return (struct uintr_ctx *)filp->private_data;
}

static inline bool uintr_active(struct uintr_percpu *p)
{
	return p->assigned_ctx != NULL;
}

static inline void uintr_signal_self(void)
{
	apic->send_IPI_self(UIPI_APIC_VECTOR);
}

#endif /* _FLUX_UINTR_H */
