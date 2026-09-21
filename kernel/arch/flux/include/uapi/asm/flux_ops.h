#ifndef _ASM_UAPI_FLUX_OPS_H
#define _ASM_UAPI_FLUX_OPS_H

struct flux_host_info;

/**
 * flux_start_kernel - starts the kernel
 *
 * @host - host information including host operations
 * @cmd_line - format for command line string that is going to be used to
 * generate the Linux kernel command line
 */
extern void flux_start_kernel(struct flux_host_info *host, const char *cmd_line,
			      ...);

#ifdef CONFIG_FLUX_SMP
/**
 * flux_start_kernel_secondary - starts the kernel on a secondary CPU
 * 
 * This function will wait for the primary CPU to finish initializing.
 *
 * @cpu - CPU number to start
 */
extern void flux_start_kernel_secondary(int cpu);
#endif

extern int flux_mpk_uaccess_init(void);

/* Runtime record updates contain no application-memory accesses. */
extern void flux_rewrite_state_lock(void);
extern void flux_rewrite_state_unlock(void);

/**
 * flux_syscall - entry point for syscalls from user space. This function
 * includes the shim layer that distinguishes between Flux-handled and
 * host-handled syscalls. Argument is passed in the standard Linux x86_64
 * syscall calling convention:
 *
 * rax: syscall number;
 * rdi, rsi, rdx, r10, r8, r9: args 1~6.
 *
 * Return: syscall return value.
 */
extern long flux_syscall(void);

/**
 * flux_syscall_dispatch - select the fast or full-context syscall entry
 *
 * Arguments use the standard Linux x86_64 syscall calling convention.
 */
extern long flux_syscall_dispatch(void);

/**
 * flux_syscall_sigreturn_nostack - rt_sigreturn entry for a restorer that
 * cannot push a synthetic return address onto a read-only signal stack
 *
 * Arguments use the standard Linux x86-64 syscall calling convention.
 */
extern long flux_syscall_sigreturn_nostack(void);

/**
 * flux_syscall_env - entry point for syscalls from user space already on kernel
 * stack without stack switch. This function is mainly intended for Flux initialization.
 * Argument is passed in the standard Linux x86_64 function calling convention:
 *
 * rdi: syscall number;
 * rsi, rdx, rcx, r8, r9, 0x8(rsp): args 1~6.
 *
 * Return: syscall return value.
 */
extern long flux_syscall_env(int nr, long arg1, long arg2, long arg3, long arg4,
			     long arg5, long arg6);

/**
 * flux_syscall_fast - entry point for syscalls from user space without restoring
 * registers. This function is mainly intended for fast syscall paths. Argument is
 * passed in the standard Linux x86_64 function callingconvention:
 * 
 * rdi: syscall number;
 * rsi, rdx, rcx, r8, r9, 0x8(rsp): args 1~6;
 *
 * Return: syscall return value.
 */
extern long flux_syscall_fast(long nr, long arg1, long arg2, long arg3,
			      long arg4, long arg5, long arg6);

/**
 * flux_signal_handler - delivers host signal to Flux
 *
 * @sig: signal number
 * @info: siginfo_t pointer
 * @ucontext: ucontext_t pointer
 * @interrupted_pkru: PKRU captured by the host signal-frame hook
 * @flux_cpu: registered Flux CPU receiving the host signal
 * @captured_uif: UIF captured and cleared by kmod before host CPL3 entry
 */
/* Return positive for a handled trap, negative for a rejected MPK operation. */
extern int flux_signal_handler(int sig, void *info, void *ucontext,
					unsigned int interrupted_pkru,
					int flux_cpu, int captured_uif);

extern long flux_kernel_exec(const char *path, char **argv, char **envp);

/**
 * flux_sys_mmap - map through LibOS Linux MM and executable-byte policy
 *
 * @addr: memory address
 * @len: memory length
 * @prot: protection flags
 * @flags: flags
 * @fd: file descriptor
 * @off: offset
 *
 * Return: mmap return value.
 */
extern long flux_sys_mmap(unsigned long addr, unsigned long len,
			  unsigned long prot, unsigned long flags,
			  unsigned long fd, unsigned long off);

/**
 * flux_kmalloc - allocate kernel memory
 *
 * @size: allocation size in bytes
 *
 * Return: pointer to allocated memory, or NULL on failure.
 */
extern void *flux_kmalloc(unsigned long size);

/**
 * flux_kfree - free kernel memory allocated by flux_kmalloc
 *
 * @addr: pointer to memory to free
 */
extern void flux_kfree(const void *addr);

/**
 * flux_is_kernel_memory - check if a pointer is in kernel memory
 *
 * @ptr: pointer to check
 *
 * Return: 1 if pointer is in kernel memory, 0 otherwise.
 */
extern int flux_is_kernel_memory(const void *ptr);

typedef enum {
	FLUX_IPI_RESCHED = 0,
	FLUX_IPI_CALLFUNC,
	FLUX_IPI_TICKBC,
	FLUX_IPI_SHUTDOWN,
	FLUX_IPI_LAST,
	FLUX_IPI_NR = FLUX_IPI_LAST
} flux_ipi_type;

/**
 * flux_uintr_handler - user interrupt handler.
 */
extern void flux_uintr_handler(void);
extern void flux_uintr_signal_stack(void);

enum {
	FLUX_UINTR_VECTOR_TIMER = 1,
	FLUX_UINTR_VECTOR_SIGNAL = 2,
	/* must be the last vector! */
	FLUX_UINTR_VECTOR_IPI = 3,
	/* more IPI vectors... */
};

struct flux_uipi_vec_info {
	int *uitt;
};

/*
 * Unified UITT tables indexed by the hardware UINTR vector number:
 *   0..FLUX_UINTR_VECTOR_IPI-1: non-IPI vectors (timer/signal/...)
 *   FLUX_UINTR_VECTOR_IPI..   : FLUX_IPI_* vectors
 */
#define FLUX_UINTR_VEC_IPI_BASE FLUX_UINTR_VECTOR_IPI
#define FLUX_UINTR_VEC_NR (FLUX_UINTR_VEC_IPI_BASE + FLUX_IPI_NR)

struct flux_uipi_pcpu {
	struct flux_uipi_vec_info vecs[FLUX_UINTR_VEC_NR];
};

#define FLUX_FD_OFFSET (512)

#endif /* _ASM_UAPI_FLUX_OPS_H */
