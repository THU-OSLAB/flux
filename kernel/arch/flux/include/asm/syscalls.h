#ifndef _ASM_FLUX_SYSCALLS_H
#define _ASM_FLUX_SYSCALLS_H

#include <asm-generic/syscalls.h>
#include <uapi/asm/host_ops.h>

struct pt_regs;

asmlinkage long sys_arch_prctl(int option, unsigned long arg2);

asmlinkage long host_syscall(long nr, ...);

asmlinkage long sys_host_mmap(unsigned long addr, unsigned long len,
			      unsigned long prot, unsigned long flags,
			      unsigned long fd, unsigned long offset);
asmlinkage long sys_host_mprotect(unsigned long start, unsigned long len,
				  unsigned long prot);
asmlinkage long sys_host_munmap(unsigned long start, unsigned long len);
asmlinkage long sys_host_brk(unsigned long brk);
asmlinkage long sys_host_mremap(unsigned long old_addr,
			       unsigned long old_len, unsigned long new_len,
			       unsigned long flags, unsigned long new_addr);
asmlinkage long sys_host_msync(unsigned long start, unsigned long len,
			      int flags);
asmlinkage long sys_host_mincore(unsigned long start, unsigned long len,
				 unsigned char __user *vec);
asmlinkage long sys_host_madvise(unsigned long start, unsigned long len,
				 int behavior);
asmlinkage long sys_host_shmat(int shmid, char __user *shmaddr, int shmflg);
asmlinkage long sys_host_shmget(int key, size_t size, int shmflg);
asmlinkage long sys_host_shmctl(int shmid, int cmd, unsigned long buf);
asmlinkage long sys_host_shmdt(char __user *shmaddr);
asmlinkage long sys_host_mlock(unsigned long start, size_t len);
asmlinkage long sys_host_munlock(unsigned long start, size_t len);
asmlinkage long sys_host_mlockall(int flags);
asmlinkage long sys_host_munlockall(void);
asmlinkage long sys_host_mlock2(unsigned long start, size_t len, int flags);
asmlinkage long sys_host_mbind(unsigned long start, unsigned long len,
			      unsigned long mode,
			      const unsigned long __user *nmask,
			      unsigned long maxnode, unsigned int flags);
asmlinkage long sys_host_swapon(const char __user *path, int flags);
asmlinkage long sys_host_swapoff(const char __user *path);
asmlinkage long sys_host_iopl(unsigned int level);
asmlinkage long sys_host_ioperm(unsigned long from, unsigned long num,
			       int turn_on);
asmlinkage long
sys_host_set_mempolicy(int mode, const unsigned long __user *nmask,
			   unsigned long maxnode);
asmlinkage long sys_host_get_mempolicy(int __user *policy,
				       unsigned long __user *nmask,
				       unsigned long maxnode,
				       unsigned long addr,
				       unsigned long flags);
asmlinkage long sys_host_process_madvise(int pidfd, unsigned long vec,
					unsigned long vlen, int behavior,
					unsigned int flags);
asmlinkage long sys_host_process_mrelease(int pidfd, unsigned int flags);
asmlinkage long sys_host_execve(const char __user *filename,
				const char __user *const __user *argv,
				const char __user *const __user *envp);
asmlinkage long sys_host_execveat(int fd, const char __user *filename,
				  const char __user *const __user *argv,
				  const char __user *const __user *envp,
				  int flags);
void flux_post_exec_to_user(void *entry, unsigned long stack);
long flux_do_host_exec(const char *path, char **argv, char **envp);
long flux_do_host_exec_with_post(const char *path, char **argv, char **envp,
				 flux_post_exec_fn_t post_exec);

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
