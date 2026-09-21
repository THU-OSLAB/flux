#ifndef _FLUX_KMOD_UINTR_H
#define _FLUX_KMOD_UINTR_H

#include <flux/host_abi.h>

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

#define XFEATURE_UINTR 14
#define XFEATURE_MASK_UINTR (1 << XFEATURE_UINTR)

#include <asm/apic.h>
#include <asm/local.h>
#include <asm/irq_vectors.h>

/* Use KVM's posted interrupt vector */
#define UIPI_APIC_VECTOR POSTED_INTR_WAKEUP_VECTOR

#define UINTR_SIGNAL_SLOT_NONE FLUX_SIGNAL_STACK_SLOTS
#define UINTR_SIGNAL_SLOT_OVERFLOW (FLUX_SIGNAL_STACK_SLOTS + 1)

enum uintr_signal_phase {
	UINTR_SIGNAL_FREE,
	UINTR_SIGNAL_CAPTURED,
	UINTR_SIGNAL_MPK_BUILDING,
	UINTR_SIGNAL_FRAME_READY,
	UINTR_SIGNAL_ACTIVE,
};

struct uintr_signal_record {
	struct task_struct *task;
	unsigned long frame;
	unsigned long stack_start;
	unsigned long stack_size;
	void __user *fpstate;
	unsigned int fpstate_size;
	u32 expected_pkru;
	enum uintr_signal_phase phase;
	bool uif;
	/* Receiver bits captured for a possible hardware frame-store fault. */
	u64 frame_fault_uirr;
};

#define MAX_NR_USER_VEC 16
struct uintr_ctx {
	unsigned long handler;
	unsigned long synthetic_handler;
	unsigned long host_fsbase;
	unsigned long signal_stack;
	unsigned long signal_handler;
	unsigned long signal_stack_slot_size;
	int logical_cpu;
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

	/* Full receiver image; signal handlers retain only sender state. */
	bool receiver_loaded;
	bool is_admin_ctx;
	u8 rt_sigreturn_slot;
	u8 sig_building_slot;
	struct uintr_xstate cur_xstate;

	struct uintr_signal_record *sig_records;
	u8 sig_uif_depth;
};

extern void uintr_cleanup_core(struct uintr_percpu *p, int cpu);
extern bool uintr_complete_signal_frame(unsigned long frame,
					bool *captured_uif,
					int *logical_cpu,
					unsigned long *host_fsbase);
extern long uintr_take_frame_fault(unsigned long frame);
extern void uintr_reclear_signal_uif(void);
extern void uintr_begin_rt_sigreturn(unsigned long frame);
extern void uintr_complete_rt_sigreturn(unsigned long return_ip);
extern void uintr_assign_core(struct uintr_ctx *ctx, u64 stack);

extern void uintr_deliver_ipi(struct uintr_percpu *p);

extern int uintr_init(void);
extern void uintr_exit(void);
extern long uintr_setup_percpu(struct file *filp, unsigned long arg);
extern void uintr_file_release(struct file *filp);

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

#endif /* _FLUX_KMOD_UINTR_H */
