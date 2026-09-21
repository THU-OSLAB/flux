#ifndef _ASM_X86_PROCESSOR_H
#define _ASM_X86_PROCESSOR_H

#ifndef __ASSEMBLY__

#include <asm/x86/cpufeatures.h>

/*
 *  CPU type and hardware bug flags. Kept separately for each CPU.
 *  Members of this structure are referenced in head_32.S, so think twice
 *  before touching them. [mj]
 */

struct cpuinfo_x86 {
	__u8 		x86; /* CPU family */
	__u8 		x86_vendor; /* CPU vendor */
	__u8 		x86_model;
	__u8 		x86_stepping;
	__u8 		x86_virt_bits;
	__u8 		x86_phys_bits;
	/* Max extended CPUID function supported: */
	__u32 		extended_cpuid_level;
	/* Maximum supported CPUID level, -1=no CPUID: */
	int 		cpuid_level;
	union {
		__u32 			x86_capability[NCAPINTS + NBUGINTS];
		unsigned long 	x86_capability_alignment;
	};
	char 		x86_vendor_id[16];
	char 		x86_model_id[64];
	int			x86_power;
	u32 		microcode;
	__u64 		xstate_features;
} __randomize_layout;

#define X86_VENDOR_INTEL 	0
#define X86_VENDOR_CYRIX 	1
#define X86_VENDOR_AMD 		2
#define X86_VENDOR_UMC 		3
#define X86_VENDOR_CENTAUR 	5
#define X86_VENDOR_TRANSMETA 7
#define X86_VENDOR_NSC 		8
#define X86_VENDOR_HYGON 	9
#define X86_VENDOR_ZHAOXIN 	10
#define X86_VENDOR_VORTEX 	11
#define X86_VENDOR_NUM 	12

#define X86_VENDOR_UNKNOWN 0xff

/*
 * capabilities of CPUs
 */
extern struct cpuinfo_x86 boot_cpu_data;
#define cpu_info boot_cpu_data
#define cpu_data(cpu) boot_cpu_data

/*
 * Query the presence of one or more xfeatures. Works on any legacy CPU as well.
 *
 * If 'feature_name' is set then put a human-readable description of
 * the feature there as well - this can be used to print error (or success)
 * messages.
 */
extern int cpu_has_xfeatures(u64 xfeatures_mask, const char **feature_name);

#include <asm/ptrace.h>

/* REP NOP (PAUSE) is a good thing to insert into busy-wait loops. */
static __always_inline void rep_nop(void)
{
	asm volatile("rep; nop" ::: "memory");
}

static __always_inline void cpu_relax(void)
{
	rep_nop();
}

static inline void prepare_to_copy(struct task_struct *tsk)
{
}

static inline unsigned long __get_wchan(struct task_struct *p)
{
	return 0;
}

void flush_thread(void);

struct thread_struct {
	unsigned long sp;
	unsigned long fsbase;
	struct pt_regs *regs;
	unsigned long fault_address;
	unsigned long fault_error;
	int fault_signal;
	int fault_code;
	unsigned int iopl_emul;
#ifdef CONFIG_FLUX_MPK
	unsigned int mpk_uaccess_depth;
	struct flux_xol_state *xol;
	struct io_bitmap *io_bitmap;
#endif
};

#define GET_TSC_CTL(adr) get_tsc_mode((adr))
#define SET_TSC_CTL(val) set_tsc_mode((val))

int get_tsc_mode(unsigned long adr);
int set_tsc_mode(unsigned int val);
bool flux_tsc_enter_kernel_mode(void);
void flux_tsc_restore_user_mode(bool restore);

extern unsigned long __end_init_task[];

#define INIT_THREAD                                                        \
	{                                                                  \
		.sp = (unsigned long)&__end_init_task -                    \
		      sizeof(struct pt_regs),                              \
		.fsbase = 0,                                               \
		.regs = (void *)&__end_init_task - sizeof(struct pt_regs), \
		.iopl_emul = 0,                                            \
	}

extern void fill_cpuinfo(struct cpuinfo_x86 *c);

/**
 * Task stack layout (high address -> low address)
 *
 *   +----------------------------------+
 *   | xstate page                      |
 *   | (PAGE_SIZE bytes)                |
 *   +----------------------------------+ <- task_stack_top()
 *   | syscall regs                  	|
 *   +----------------------------------+ THREAD_SIZE / 2 bytes
 *   | syscall stack             		|
 *   | ...                              |
 *   +----------------------------------+ <- uintr stack top
 *   | ...                              |
 *   | uintr stack                      | THREAD_SIZE / 2 bytes
 *   | ...                              |
 *   +----------------------------------+ <- task stack base
 */
#define task_xstate(p) (task_stack_page(p) + THREAD_SIZE - PAGE_SIZE)
#define task_stack_top(p) ((unsigned long)(task_xstate(p)))
#define uintr_stack_top(p) \
	((unsigned long)(task_stack_page(p) + (THREAD_SIZE / 2)))
#define task_sys_regs(p) ((void *)task_stack_top(p) - sizeof(struct pt_regs))
#define task_pt_regs(p) (p->thread.regs)

/* write gs base */
static __always_inline void wrgsbase(u64 val)
{
	asm volatile("wrgsbase %0" : : "r"(val));
}

/* read physical cpu id through rdtscp */
static inline int get_host_cpu_id(void)
{
	unsigned int eax, ecx, edx;
	asm volatile("rdtscp" : "=a"(eax), "=d"(edx), "=c"(ecx));
	return ecx & 0xff;
}

static inline void wrfsbase(u64 val)
{
	asm volatile("wrfsbase %0" : : "r"(val));
}

static inline u64 rdfsbase(void)
{
	u64 val;

	asm volatile("rdfsbase %0" : "=r"(val));

	return val;
}

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_PROCESSOR_H */
