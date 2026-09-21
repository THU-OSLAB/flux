#ifndef _ASM_FLUX_SYSCALLS_H
#define _ASM_FLUX_SYSCALLS_H

#include <asm-generic/syscalls.h>
#include <uapi/asm/host_ops.h>
#include <linux/types.h>
#include <linux/sched.h>

struct pt_regs;
struct perf_event_attr;

asmlinkage long sys_arch_prctl(int option, unsigned long arg2);
asmlinkage long sys_flux_perf_event_open(
		struct perf_event_attr __user *attr_uptr, pid_t pid, int cpu,
		int group_fd, unsigned long flags);


asmlinkage long host_syscall(long nr, ...);

asmlinkage long sys_host_iopl(unsigned int level);
asmlinkage long sys_host_ioperm(unsigned long from, unsigned long num,
				int turn_on);
asmlinkage long sys_flux_madvise(unsigned long start, size_t len_in, int behavior);
asmlinkage long sys_flux_pkey_mprotect(unsigned long start, size_t len,
				       unsigned long prot, int pkey);
asmlinkage long sys_flux_pkey_alloc(unsigned long flags,
				    unsigned long init_val);
asmlinkage long sys_flux_pkey_free(int pkey);
asmlinkage long sys_flux_mprotect(unsigned long start, size_t len,
				 unsigned long prot);
asmlinkage long sys_flux_munmap(unsigned long addr, size_t len);
extern const char *flux_elf_interpreter;

#ifdef CONFIG_FLUX_FAST_NET

struct sockaddr;
struct user_msghdr;

asmlinkage long sys_fast_socket(int family, int type, int protocol);
asmlinkage long sys_fast_listen(int fd, int backlog);
asmlinkage long sys_fast_bind(int fd, struct sockaddr __user *umyaddr,
			      int addrlen);
asmlinkage long sys_fast_connect(int fd, struct sockaddr __user *uservaddr,
				 int addrlen);
asmlinkage long sys_fast_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
				 int __user *upeer_addrlen, int flags);
asmlinkage long sys_fast_accept(int fd, struct sockaddr __user *upeer_sockaddr,
				int __user *upeer_addrlen);
asmlinkage long sys_fast_shutdown(int fd, int how);
asmlinkage long sys_fast_sendto(int fd, void __user *buff, size_t len,
				unsigned int flags,
				struct sockaddr __user *addr,
				int __user *addr_len);
asmlinkage long sys_fast_recvfrom(int fd, void __user *ubuf, size_t size,
				  unsigned int flags,
				  struct sockaddr __user *addr,
				  int __user *addr_len);
asmlinkage long sys_fast_sendmsg(int fd, struct user_msghdr __user *msg,
				 unsigned int flags);
asmlinkage long sys_fast_recvmsg(int fd, struct user_msghdr __user *msg,
				 unsigned int flags);
asmlinkage long sys_fast_getsockopt(int fd, int level, int optname,
				    char __user *optval, int __user *optlen);
asmlinkage long sys_fast_setsockopt(int fd, int level, int optname,
				    char __user *user_optval, int optlen);
asmlinkage long sys_fast_getsockname(int fd, struct sockaddr __user *usockaddr,
				     int __user *usockaddr_len);
asmlinkage long sys_fast_getpeername(int fd, struct sockaddr __user *usockaddr,
				     int __user *uaddrlen);
#endif

#endif /* _ASM_FLUX_SYSCALLS_H */
