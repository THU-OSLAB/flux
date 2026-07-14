#define pr_fmt(fmt) "flux_mm: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/pid.h>
#include <asm/syscall.h>
#include <asm/mmu_context.h>
#include <linux/sched/signal.h>

#include "dev.h"
#include "hook.h"
#include "mm.h"
#include "mpk.h"

struct flux_mm_range {
	unsigned long start;
	unsigned long end;
	struct list_head list;
};

static LIST_HEAD(flux_mm_ctx_registry);
static DEFINE_MUTEX(flux_mm_ctx_registry_lock);

static struct flux_mm_ctx *flux_mm_ctx_find_locked(pid_t tgid)
{
	struct flux_mm_ctx *ctx;

	list_for_each_entry(ctx, &flux_mm_ctx_registry, registry_node) {
		if (ctx->owner_tgid == tgid)
			return ctx;
	}

	return NULL;
}

static int flux_mm_ctx_register(struct flux_mm_ctx *ctx)
{
	int err = 0;

	mutex_lock(&flux_mm_ctx_registry_lock);
	if (flux_mm_ctx_find_locked(ctx->owner_tgid)) {
		err = -EEXIST;
	} else {
		list_add_tail_rcu(&ctx->registry_node, &flux_mm_ctx_registry);
	}
	mutex_unlock(&flux_mm_ctx_registry_lock);

	return err;
}

static void flux_mm_ctx_unregister(struct flux_mm_ctx *ctx)
{
	bool removed = false;

	mutex_lock(&flux_mm_ctx_registry_lock);
	if (!list_empty(&ctx->registry_node)) {
		list_del_rcu(&ctx->registry_node);
		removed = true;
	}
	mutex_unlock(&flux_mm_ctx_registry_lock);
	if (removed) {
		synchronize_rcu();
		INIT_LIST_HEAD(&ctx->registry_node);
	}
}

bool notrace flux_mm_mpk_enabled_current_rcu(void)
{
	struct flux_mm_ctx *ctx;
	pid_t tgid = task_tgid_nr(current);
	bool enabled = false;

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &flux_mm_ctx_registry, registry_node) {
		if (READ_ONCE(ctx->owner_tgid) == tgid) {
			enabled = READ_ONCE(ctx->mpk_enabled);
			break;
		}
	}
	rcu_read_unlock();
	return enabled;
}

struct flux_mm_ctx *flux_mm_ctx_get_current(void)
{
	struct flux_mm_ctx *ctx = NULL;
	struct pid *leader_pid;

	leader_pid = get_task_pid(current->group_leader, PIDTYPE_PID);
	if (!leader_pid)
		return NULL;

	mutex_lock(&flux_mm_ctx_registry_lock);
	ctx = flux_mm_ctx_find_locked(task_tgid_nr(current));
	if (ctx && !kref_get_unless_zero(&ctx->refcount))
		ctx = NULL;
	mutex_unlock(&flux_mm_ctx_registry_lock);
	put_pid(leader_pid);

	return ctx;
}

void flux_mm_ctx_put(struct flux_mm_ctx *ctx)
{
	if (ctx)
		kref_put(&ctx->refcount, flux_mm_ctx_release);
}

static void flux_mm_ranges_clear(struct list_head *head)
{
	struct flux_mm_range *range, *tmp;

	list_for_each_entry_safe(range, tmp, head, list) {
		list_del(&range->list);
		kfree(range);
	}
}

static bool flux_mm_range_list_overlaps(struct list_head *head,
					unsigned long start, unsigned long end)
{
	struct flux_mm_range *range;

	list_for_each_entry(range, head, list) {
		if (start < range->end && end > range->start)
			return true;
	}

	return false;
}

static int flux_mm_ranges_snapshot(struct mm_struct *mm, struct list_head *head)
{
	struct vma_iterator vmi;
	struct vm_area_struct *vma;
	struct flux_mm_range *range;
	int err = 0;

	if (mmap_read_lock_killable(mm))
		return -EINTR;

	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma)
	{
		range = kmalloc(sizeof(*range), GFP_KERNEL);
		if (!range) {
			err = -ENOMEM;
			break;
		}
		range->start = vma->vm_start;
		range->end = vma->vm_end;
		list_add_tail(&range->list, head);
	}

	if (err)
		flux_mm_ranges_clear(head);

	mmap_read_unlock(mm);

	return err;
}

static int flux_mm_record_base_maps(struct flux_mm_ctx *ctx,
				    struct mm_struct *mm)
{
	LIST_HEAD(ranges);
	int err;

	err = flux_mm_ranges_snapshot(mm, &ranges);
	if (err)
		return err;

	mutex_lock(&ctx->lock);
	flux_mm_ranges_clear(&ctx->base_maps);
	list_splice_tail_init(&ranges, &ctx->base_maps);
	mutex_unlock(&ctx->lock);

	return 0;
}

static void flux_mm_ctx_reset_ranges(struct flux_mm_ctx *ctx)
{
	flux_mm_ranges_clear(&ctx->base_maps);
	flux_mm_ranges_clear(&ctx->clean_maps);
}
static struct flux_hook_group execve_hook_group;

static atomic_t flux_tmpfile_id = ATOMIC_INIT(0);
static struct kmem_cache *flux_proc_cache;

static int flux_unlink_open_file(struct file *file)
{
	struct dentry *dentry = file->f_path.dentry;
	struct dentry *dir;
	struct mnt_idmap *idmap;
	int err;

	err = mnt_want_write(file->f_path.mnt);
	if (err)
		return err;

	dir = dget_parent(dentry);
	dget(dentry);
	inode_lock_nested(d_inode(dir), I_MUTEX_PARENT);
	idmap = file_mnt_idmap(file);
	err = vfs_unlink(idmap, d_inode(dir), dentry, NULL);
	inode_unlock(d_inode(dir));
	dput(dentry);
	dput(dir);
	mnt_drop_write(file->f_path.mnt);

	return err;
}

static struct flux_proc *flux_proc_alloc(gfp_t flags)
{
	if (WARN_ON(!flux_proc_cache))
		return NULL;

	return kmem_cache_zalloc(flux_proc_cache, flags);
}

static void flux_proc_free(struct flux_proc *proc)
{
	kmem_cache_free(flux_proc_cache, proc);
}

static int dup_ro_file(struct file *file, unsigned long len,
		       unsigned long pgoff)
{
	char filename[256];
	char *buf;
	int tmpfd, err;
	struct file *tmpfile;
	loff_t rpos = pgoff, wpos = 0;
	ssize_t bytes;
	unsigned long rem = len;
	int flags = O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC | O_DIRECT;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		pr_err("failed to allocate buffer\n");
		return -ENOMEM;
	}

	tmpfd = get_unused_fd_flags(flags);
	if (tmpfd < 0) {
		pr_err("failed to get unused fd\n");
		err = -EBADFD;
		goto out_fd;
	}

	snprintf(filename, sizeof(filename), "/tmp/flux_mmap_%d_%d",
		 current->pid, atomic_fetch_add(1, &flux_tmpfile_id));
	tmpfile = filp_open(filename, flags, 0755);
	if (IS_ERR(tmpfile)) {
		pr_err("failed to create temp file\n");
		err = -EBADF;
		goto out_open;
	} else {
		fd_install(tmpfd, tmpfile);
		err = flux_unlink_open_file(tmpfile);
		if (err)
			pr_warn("failed to unlink temp file %s: %d\n", filename,
				err);
	}

	while (rem > 0 && (bytes = kernel_read(file, buf, min(rem, PAGE_SIZE),
					       &rpos)) > 0) {
		if (kernel_write(tmpfile, buf, bytes, &wpos) != bytes) {
			pr_err("failed to write temp file\n");
			err = -EIO;
			goto out_write;
		}
		rem -= bytes;
	}

	pr_debug("dup file %s fd %d (len %lu bytes)\n", filename, tmpfd, len);

	kfree(buf);
	return tmpfd;
out_write:
	filp_close(tmpfile, NULL);
out_open:
	put_unused_fd(tmpfd);
out_fd:
	kfree(buf);
	return err;
}

static asmlinkage long (*orig_sys_mmap)(struct pt_regs *regs);
static long hook_sys_mmap(struct pt_regs *regs)
{
	int prot = regs->dx;
	int flags = regs->r10;
	int fd = regs->r8, tmpfd;
	struct file *file = NULL;

	if (!test_thread_flag(TIF_HOOK_ACTIVE))
		goto out;

	/*
	 * Share all writable mappings.
	 *
	 * MAP_EXECUTABLE and MAP_DENYWRITE are completely ignored throughout the
	 * kernel.
	 */
	if (flags & MAP_PRIVATE) {
		if ((prot == PROT_NONE) || (prot & PROT_WRITE)) {
			file = fget(fd);
			if (file) {
				/* cannot dup a non-readable file */
				if (!(file->f_mode & FMODE_READ)) {
					fput(file);
					goto out;
				}

				/* dup a non-writable file */
				if (!(file->f_mode & FMODE_WRITE)) {
					tmpfd = dup_ro_file(file, regs->si,
							    regs->r9);
					if (tmpfd < 0) {
						fput(file);
						goto out;
					}
					regs->r8 = tmpfd;
					regs->r9 = 0;
				}
				fput(file);
			}

			flags &= ~MAP_PRIVATE;
			flags |= MAP_SHARED;

			pr_debug(
				"mmap(addr=0x%lx, len=%lu, prot=0x%lx, flags=0x%lx, fd=%lu, off=0x%lx)  MAP_PRIVATE -> MAP_SHARED",
				regs->di, // addr
				regs->si, // length
				regs->dx, // prot
				regs->r10, // flags
				regs->r8, // fd
				regs->r9); // offset
		}
	}

	regs->r10 = flags;
out:
	return orig_sys_mmap(regs);
}

static asmlinkage long (*orig_sys_brk)(struct pt_regs *regs);
static long hook_sys_brk(struct pt_regs *regs)
{
	unsigned long old_brk, new_brk, addr;

	if (!test_thread_flag(TIF_HOOK_ACTIVE))
		return orig_sys_brk(regs);

	old_brk = current->mm->brk;
	if (regs->di && regs->di < old_brk) {
		pr_debug("reject brk shrink from 0x%lx to 0x%lx\n",
			 old_brk, regs->di);
		return old_brk;
	}

	new_brk = orig_sys_brk(regs);

	pr_debug("brk(addr=0x%lx)=0x%lx\n", regs->di, new_brk);

	/* remap the brk with MAP_SHARED */
	if (new_brk > old_brk) {
		addr = vm_mmap(NULL, old_brk, new_brk - old_brk,
			       PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, 0);
		if (IS_ERR_VALUE(addr) || addr != old_brk) {
			pr_err("failed to remap brk with MAP_SHARED\n");
			return -ENOMEM;
		}
	}

	return new_brk;
}

static struct flux_ftrace_hook mmap_hooks[] = {
	FLUX_HOOK("__x64_sys_mmap", hook_sys_mmap, &orig_sys_mmap),
	FLUX_HOOK("__x64_sys_brk", hook_sys_brk, &orig_sys_brk),
};

static struct flux_hook_group mmap_hook_group = {
	.name = "mmap",
	.hooks = mmap_hooks,
	.nr_hooks = ARRAY_SIZE(mmap_hooks),
};

int flux_mm_hook_install(void)
{
	if (test_thread_flag(TIF_HOOK_ACTIVE))
		return 0;

	set_thread_flag(TIF_HOOK_ACTIVE);
	return 0;
}

int flux_mm_hook_remove(void)
{
	struct flux_mm_ctx *ctx;
	struct mm_struct *mm = current->mm;
	int err = 0;

	if (mm) {
		ctx = flux_mm_ctx_get_current();
		if (!ctx) {
			err = -ENOENT;
			pr_err("failed to find mm ctx for leader pid=%d\n",
			       task_pid_nr(current->group_leader));
		} else {
			err = flux_mm_record_base_maps(ctx, mm);
			if (err)
				pr_err("failed to record base mm maps: %d\n",
				       err);
			flux_mm_ctx_put(ctx);
		}
	}

	if (!test_thread_flag(TIF_HOOK_ACTIVE))
		return err;

	clear_thread_flag(TIF_HOOK_ACTIVE);
	return err;
}

int __init flux_mm_hook_init(void)
{
	int err;

	flux_proc_cache = kmem_cache_create("flux_proc_cache",
					    sizeof(struct flux_proc), 0,
					    SLAB_HWCACHE_ALIGN, NULL);
	if (!flux_proc_cache)
		return -ENOMEM;
	err = flux_hook_group_install(&mmap_hook_group);
	if (err) {
		pr_err("failed to install mmap hooks: %d\n", err);
		goto out_cache;
	}

	err = flux_hook_group_install(&execve_hook_group);
	if (err) {
		pr_err("failed to install execve hooks: %d\n", err);
		goto out_mmap_hooks;
	}

	return 0;

out_mmap_hooks:
	flux_hook_group_remove(&mmap_hook_group);
out_cache:
	kmem_cache_destroy(flux_proc_cache);
	flux_proc_cache = NULL;
	return err;
}

void flux_mm_hook_exit(void)
{
	flux_hook_group_remove(&execve_hook_group);
	flux_hook_group_remove(&mmap_hook_group);
	if (flux_proc_cache) {
		kmem_cache_destroy(flux_proc_cache);
		flux_proc_cache = NULL;
	}
}

static unsigned long (*orig_vm_mmap)(struct file *file, unsigned long addr,
				     unsigned long len, unsigned long prot,
				     unsigned long flag, unsigned long offset);
static unsigned long hook_vm_mmap(struct file *file, unsigned long addr,
				  unsigned long len, unsigned long prot,
				  unsigned long flag, unsigned long offset)
{
	unsigned long ret;
	int tmpfd;
	struct file *tmpfile = NULL;

	if (!test_thread_flag(TIF_HOOK_ACTIVE))
		return orig_vm_mmap(file, addr, len, prot, flag, offset);

	pr_debug(
		"vm_mmap(addr=0x%lx, len=%lu, prot=0x%lx, flags=0x%lx, ino=%lu, off=0x%lx)\n",
		addr, len, prot, flag, file ? file->f_inode->i_ino : 0, offset);

	if (flag & MAP_PRIVATE) {
		if ((prot == PROT_NONE) || (prot & PROT_WRITE)) {
			if (file) {
				tmpfd = dup_ro_file(file, len, offset);
				if (tmpfd < 0)
					return -ENOMEM;
				tmpfile = fget(tmpfd);
				if (!tmpfile)
					return -EBADF;
			}

			flag &= ~MAP_PRIVATE;
			flag |= MAP_SHARED;
		}
	}

	if (tmpfile) {
		ret = orig_vm_mmap(tmpfile, addr, len, prot, flag, 0);
		fput(tmpfile);
		return ret;
	}

	return orig_vm_mmap(file, addr, len, prot, flag, offset);
}

static int (*orig_vm_brk_flags)(unsigned long addr, unsigned long request,
				unsigned long flags);
static int hook_vm_brk_flags(unsigned long addr, unsigned long request,
			     unsigned long flags)
{
	int ret;
	unsigned long new_addr;

	if (!test_thread_flag(TIF_HOOK_ACTIVE))
		return orig_vm_brk_flags(addr, request, flags);

	pr_debug("vm_brk_flags(addr=0x%lx, request=%lu, flags=0x%lx)\n", addr,
		 request, flags);

	ret = orig_vm_brk_flags(addr, request, flags);
	if (request > 0 && !ret && !(flags & VM_EXEC)) {
		new_addr = vm_mmap(NULL, addr, request, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, 0);
		if (IS_ERR_VALUE(new_addr) || new_addr != addr) {
			pr_err("failed to remap brk with MAP_SHARED\n");
			return -ENOMEM;
		}
	}
	return ret;
}

static struct flux_ftrace_hook execve_hooks[] = {
	FLUX_HOOK("vm_mmap", hook_vm_mmap, &orig_vm_mmap),
	FLUX_HOOK("vm_brk_flags", hook_vm_brk_flags, &orig_vm_brk_flags),
};

static struct flux_hook_group execve_hook_group = {
	.name = "execve",
	.hooks = execve_hooks,
	.nr_hooks = ARRAY_SIZE(execve_hooks),
};

void flux_dump_vmas(void)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	struct vma_iterator vmi;
	struct file *file;
	loff_t pos = 0;
	char path[64];
	char line[256];
	u16 rnd = get_random_u16();
	pid_t pid = task_pid_nr(current);
	ssize_t wr;
	int len;

	len = scnprintf(path, sizeof(path), "/tmp/flux_vmas_%d_%u", pid, rnd);
	if (len <= 0 || len >= sizeof(path)) {
		pr_err("failed to build vma dump path\n");
		return;
	}

	file = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(file)) {
		pr_err("failed to open %s for vma dump: %ld\n", path,
		       PTR_ERR(file));
		return;
	}

	if (mmap_read_lock_killable(mm))
		goto out_close;

	len = scnprintf(line, sizeof(line), "All VMAs:\n");
	wr = kernel_write(file, line, len, &pos);
	if (wr != len)
		goto out_unlock;
	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma)
	{
		if (vma->vm_file) {
			len = scnprintf(
				line, sizeof(line),
				"  [%16lx-%16lx] flags=0x%7lx %s%s[file: %pD4]\n",
				vma->vm_start, vma->vm_end, vma->vm_flags,
				(vma->vm_flags & VM_GROWSDOWN) ? "[stack] " :
								 "",
				(vma->vm_flags & VM_SHARED) ? "[shared] " : "",
				vma->vm_file);
		} else {
			len = scnprintf(
				line, sizeof(line),
				"  [%16lx-%16lx] flags=0x%7lx %s%s[anon]%s\n",
				vma->vm_start, vma->vm_end, vma->vm_flags,
				(vma->vm_flags & VM_GROWSDOWN) ? "[stack] " :
								 "",
				(vma->vm_flags & VM_SHARED) ? "[shared] " : "",
				(vma->vm_flags & VM_WRITE) &&
						!(vma->vm_flags & VM_SHARED) ?
					" [ERR!]" :
					"");
		}

		wr = kernel_write(file, line, len, &pos);
		if (wr != len)
			goto out_unlock;
	}

	pr_debug("VMA dump saved to %s\n", path);
out_unlock:
	mmap_read_unlock(mm);
out_close:
	filp_close(file, NULL);
}

static void flux_remap_stack(void)
{
	unsigned long stack_top, stack_shift;
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	unsigned long start, end, addr;
	void *buf;

	if (mmap_read_lock_killable(mm))
		return;

	/* search vma and remap it */
	stack_top = mm->start_stack;
	vma = find_vma(mm, stack_top - 1);
	if (!vma || !(vma->vm_flags & VM_STACK)) {
		pr_err("failed to find stack VMA for stack top %lx\n",
		       stack_top);
		mmap_read_unlock(mm);
		return;
	}

	mmap_read_unlock(mm);

	pr_debug("remap stack %lx VMA [%lx-%lx] flags=0x%lx\n", stack_top,
		 vma->vm_start, vma->vm_end, vma->vm_flags);

	start = vma->vm_start;
	end = vma->vm_end;
	stack_shift = end - stack_top;

	buf = kmalloc(stack_shift, GFP_KERNEL);
	if (!buf) {
		pr_err("failed to allocate buffer\n");
		return;
	}

	/* copy stack to buffer */
	if (copy_from_user(buf, (void __user *)stack_top, stack_shift)) {
		pr_err("failed to copy stack to buffer\n");
		goto out_free;
	}

	/* remap stack with MAP_SHARED */
	addr = vm_mmap(NULL, start, end - start, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, 0);
	if (IS_ERR_VALUE(addr) || addr != start) {
		pr_err("failed to remap stack with MAP_SHARED %lx\n", addr);
		goto out_free;
	}

	/* copy back from buffer to remapped stack */
	if (copy_to_user((void __user *)stack_top, buf, stack_shift)) {
		pr_err("failed to copy back to remapped stack\n");
		goto out_unmap;
	}

	goto out_free;
out_unmap:
	vm_munmap(start, end - start);
out_free:
	kfree(buf);
}

static void *resolve_execve_symbol(void)
{
	void *sys_call_table;

	sys_call_table = (void *)flux_lookup_symbol("sys_call_table");
	if (!sys_call_table) {
		pr_err("failed to resolve sys_call_table symbol\n");
		return NULL;
	}

	return *(void **)(sys_call_table + __NR_execve * sizeof(void *));
}

/**
 * Execute a new program.
 *
 * @param arg: the argument of the execve system call
 * @return 0 on success, otherwise an error code
 */
int flux_execve(unsigned long arg)
{
	struct pt_regs regs;
	struct flux_execve_args args;
	long (*sys_execve)(struct pt_regs *);
	long ret;
	bool map_shared = test_thread_flag(TIF_HOOK_ACTIVE);

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	pr_debug("execve: filename=%lx, argv=%lx, envp=%lx\n",
		 (unsigned long)args.filename, (unsigned long)args.argv,
		 (unsigned long)args.envp);

	sys_execve = resolve_execve_symbol();
	if (!sys_execve) {
		pr_err("failed to resolve sys_execve symbol\n");
		return -ENOENT;
	}

	/* execve will not use the temporary regs */
	regs = *current_pt_regs();
	regs.di = (unsigned long)args.filename;
	regs.si = (unsigned long)args.argv;
	regs.dx = (unsigned long)args.envp;

	ret = sys_execve(&regs);
	if (map_shared) {
		flux_remap_stack();
		flux_dump_vmas();
	}
	return ret;
}

static struct mm_struct *(*flux_dup_mm)(struct task_struct *,
					struct mm_struct *);
static void (*flux_switch_mm_irqs_off)(struct mm_struct *, struct mm_struct *,
				       struct task_struct *);

static void *resolve_dup_mm_symbol(void)
{
	void *dup_mm;

	dup_mm = (void *)flux_lookup_symbol("dup_mm");
	if (!dup_mm) {
		pr_err("failed to resolve dup_mm symbol\n");
		return NULL;
	}

	return dup_mm;
}

static void *resolve_switch_mm_symbol(void)
{
	void *switch_mm;

	switch_mm = (void *)flux_lookup_symbol("switch_mm_irqs_off");
	if (!switch_mm) {
		pr_err("failed to resolve switch_mm_irqs_off symbol\n");
		return NULL;
	}

	return switch_mm;
}

static void flux_proc_reset_stats(struct flux_proc *proc)
{
	proc->min_flt = 0;
	proc->maj_flt = 0;
	proc->nvcsw = 0;
	proc->nivcsw = 0;
}

static void flux_proc_save_stats(struct flux_proc *proc,
				 const struct task_struct *task)
{
	proc->min_flt = task->min_flt;
	proc->maj_flt = task->maj_flt;
	proc->nvcsw = task->nvcsw;
	proc->nivcsw = task->nivcsw;
}

static void flux_proc_restore_stats(const struct flux_proc *proc,
				    struct task_struct *task)
{
	task->min_flt = proc->min_flt;
	task->maj_flt = proc->maj_flt;
	task->nvcsw = proc->nvcsw;
	task->nivcsw = proc->nivcsw;
}

static void flux_proc_init(struct flux_proc *proc, int count,
			   struct mm_struct *mm)
{
	proc->key = 0;
	atomic_set(&proc->count, count);
	proc->mm = mm;
	flux_proc_reset_stats(proc);
}

static void flux_proc_release(struct flux_mm_ctx *ctx, int proc_key,
			      struct flux_proc *proc)
{
	if (WARN_ON_ONCE(proc_key == 0))
		return;

	if (proc->mm)
		mmput(proc->mm);
	proc->mm = NULL;
	WARN_ON(xa_erase(&ctx->procs, proc_key) != proc);
	flux_proc_free(proc);
}

void flux_proc_put(struct flux_mm_ctx *ctx, int proc_key,
		   struct flux_proc *proc)
{
	if (atomic_dec_and_test(&proc->count)) {
		flux_proc_release(ctx, proc_key, proc);
	}
}

static struct flux_mm_ctx *alloc_mm_ctx(void)
{
	struct flux_mm_ctx *ctx;
	struct mm_struct *mm = current->mm;
	int err;

	if (unlikely(!mm))
		return NULL;

	ctx = kzalloc(sizeof(struct flux_mm_ctx), GFP_KERNEL);
	if (unlikely(!ctx))
		return NULL;

	/*
	 * proc0 persists after the host task switches away from the original mm,
	 * so it needs its own lifetime ref just like duplicated mms do.
	 */
	mmget(mm);

	xa_init_flags(&ctx->procs, XA_FLAGS_ALLOC);
	flux_proc_init(&ctx->proc0, 1 + flux_shm->used_cpus, mm);
	xa_store(&ctx->procs, 0, &ctx->proc0, GFP_KERNEL);

	kref_init(&ctx->refcount);
	mutex_init(&ctx->lock);
	ctx->owner_tgid = task_tgid_nr(current);
	INIT_LIST_HEAD(&ctx->base_maps);
	INIT_LIST_HEAD(&ctx->clean_maps);
	INIT_LIST_HEAD(&ctx->registry_node);

	err = flux_mm_ctx_register(ctx);
	if (unlikely(err)) {
		mmput(mm);
		kfree(ctx);
		return NULL;
	}

	return ctx;
}

void flux_mm_ctx_release(struct kref *ref)
{
	struct flux_mm_ctx *ctx =
		container_of(ref, struct flux_mm_ctx, refcount);
	struct flux_proc *proc;
	unsigned long proc_key;

	flux_mpk_ctx_release(ctx);

	xa_for_each_start(&ctx->procs, proc_key, proc, 1) {
		flux_proc_release(ctx, proc_key, proc);
	}

	flux_mm_ctx_unregister(ctx);
	mutex_lock(&ctx->lock);
	flux_mm_ctx_reset_ranges(ctx);
	ctx->clean_mm = NULL;
	mutex_unlock(&ctx->lock);

	if (ctx->proc0.mm)
		mmput(ctx->proc0.mm);

	kfree(ctx);
}

int flux_mm_setup_ctx(struct file *filp)
{
	struct flux_mm_ctx *ctx;

	if (filp->private_data)
		return -EINVAL;

	ctx = alloc_mm_ctx();
	if (!ctx)
		return -ENOMEM;

	filp->private_data = ctx;
	return 0;
}

/**
 * Copy the memory of the current process to the target process.
 *
 * @param ctx: the context of the memory
 * @param proc_key_ptr: the pointer to the key of the target process
 * @return 0 on success, otherwise an error code
 */
int flux_copy_mm(struct flux_mm_ctx *ctx, int __user *proc_key_ptr)
{
	struct flux_proc *proc;
	struct mm_struct *mm = NULL, *oldmm;
	int proc_key = -1, err = -ENOMEM;

	if (unlikely(!flux_dup_mm))
		flux_dup_mm = resolve_dup_mm_symbol();
	if (!flux_dup_mm) {
		pr_err("failed to resolve dup_mm symbol\n");
		return -ENOENT;
	}

	proc = flux_proc_alloc(GFP_KERNEL);
	if (!proc)
		return -ENOMEM;

	flux_proc_init(proc, 1, NULL);

	err = -ENOMEM;
	oldmm = current->mm;
	if (!oldmm)
		goto out;

	/* we bind new mm to current process  */
	mm = flux_dup_mm(current, oldmm);
	if (!mm) {
		pr_err("failed to duplicate mm_struct\n");
		goto out;
	}

	proc->mm = mm;

	/* Insert only after proc is fully initialized, so mm_ctx_release
	 * never sees a half-initialized proc (avoids use-after-free).
	 * Key 0 is reserved for primary process. */
	if ((err = xa_alloc(&ctx->procs, &proc_key, proc, flux_proc_xa_limit,
			    GFP_KERNEL)) < 0) {
		pr_err("failed to allocate proc_key: %d\n", err);
		goto out_mm;
	}
	proc->key = proc_key;

	if (copy_to_user(proc_key_ptr, &proc_key, sizeof(u32))) {
		err = -EFAULT;
		goto out_xa;
	}

	pr_debug("create new mm for proc_key=%d from pid=%d\n", proc_key,
		 current->pid);

	return 0;
out_xa:
	xa_erase(&ctx->procs, proc_key);
out_mm:
	mmput(mm);
out:
	flux_proc_free(proc);
	return err;
}

/**
 * Release the memory of the target process.
 *
 * @param ctx: the context of the memory
 * @param proc_key: the key of the target process
 * @return 0 on success, otherwise an error code
 */
int flux_release_mm(struct flux_mm_ctx *ctx, int proc_key)
{
	struct flux_proc *proc;

	pr_debug("release mm for proc_key=%d\n", proc_key);

	if (proc_key <= 0)
		return -EINVAL;

	proc = xa_load(&ctx->procs, proc_key);
	if (!proc)
		return -ENOENT;

	flux_proc_put(ctx, proc_key, proc);

	return 0;
}

/**
 * Switch the memory of the current process to the target process.
 *
 * @param ctx: the context of the memory
 * @param proc_key_to: the key of the target process
 * @param proc_key_from: the key of the current process
 * @return 0 on success, otherwise an error code
 */
int flux_switch_mm(struct flux_mm_ctx *ctx, int proc_key_to, int proc_key_from)
{
	struct flux_proc *proc_to, *proc_from;
	struct mm_struct *mm_to = NULL, *mm_from = NULL;
	struct task_struct *p = current;
	int err = 0;

	if (unlikely(!flux_switch_mm_irqs_off))
		flux_switch_mm_irqs_off = resolve_switch_mm_symbol();
	if (!flux_switch_mm_irqs_off) {
		pr_err("failed to resolve switch_mm_irqs_off symbol\n");
		return -ENOENT;
	}

	if (proc_key_to < 0 || proc_key_from < 0 ||
	    proc_key_to == proc_key_from)
		return -EINVAL;

	proc_to = xa_load(&ctx->procs, proc_key_to);
	proc_from = xa_load(&ctx->procs, proc_key_from);
	if (!proc_to || !proc_from) {
		err = -ENOENT;
		goto out;
	}

	mm_to = proc_to->mm;
	mm_from = proc_from->mm;
	if (!mm_to || !mm_from) {
		err = -ENOENT;
		goto out;
	}

	mmget(mm_to);
	flux_proc_get(proc_to);

	task_lock(p);
	flux_proc_save_stats(proc_from, p);
	flux_proc_restore_stats(proc_to, p);
	p->mm = p->active_mm = mm_to;
	task_unlock(p);

	local_irq_disable();
	flux_switch_mm_irqs_off(mm_from, mm_to, p);
	lru_gen_use_mm(mm_to);
	local_irq_enable();

	flux_proc_put(ctx, proc_key_from, proc_from);
	mmput(mm_from);

out:
	return err;
}

/**
 * Prepare to clean the memory of the current process.
 *
 * @param ctx: the context of the memory
 * @return 0 on success, otherwise an error code
 */
static int flux_clean_mm_pre(struct flux_mm_ctx *ctx)
{
	struct mm_struct *mm = current->mm;
	struct vma_iterator vmi;
	struct vm_area_struct *vma;
	struct flux_mm_range *range;
	int err = 0;

	if (!mm)
		return -EINVAL;

	if (mmap_read_lock_killable(mm))
		return -EINTR;

	mutex_lock(&ctx->lock);

	/* There is a clean mm in progress, wait for it to finish. */
	if (ctx->clean_mm != NULL) {
		err = -EAGAIN;
		goto out_mm;
	}

	flux_mm_ranges_clear(&ctx->clean_maps);
	ctx->clean_mm = mm;

	if (list_empty(&ctx->base_maps)) {
		err = -ENOENT;
		goto out_mm;
	}

	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma)
	{
		if (flux_mm_range_list_overlaps(&ctx->base_maps, vma->vm_start,
						vma->vm_end))
			continue;

		range = kmalloc(sizeof(*range), GFP_KERNEL);
		if (!range) {
			err = -ENOMEM;
			break;
		}
		range->start = vma->vm_start;
		range->end = vma->vm_end;
		list_add_tail(&range->list, &ctx->clean_maps);
	}

	if (err) {
		flux_mm_ranges_clear(&ctx->clean_maps);
		ctx->clean_mm = NULL;
	}

out_mm:
	mutex_unlock(&ctx->lock);
	mmap_read_unlock(mm);
	return err;
}

/**
 * Clean the memory of the current process.
 *
 * @param ctx: the context of the memory
 * @return 0 on success, otherwise an error code
 */
static int flux_clean_mm_post(struct flux_mm_ctx *ctx)
{
	struct mm_struct *mm = current->mm;
	struct flux_mm_range *range, *tmp;
	int err = 0;
	int ret;

	if (!mm)
		return -EINVAL;

	mutex_lock(&ctx->lock);

	if (ctx->clean_mm != mm) {
		err = -EINVAL;
		goto out_unlock;
	}

	list_for_each_entry_safe(range, tmp, &ctx->clean_maps, list) {
		ret = vm_munmap(range->start, range->end - range->start);
		if (ret)
			pr_err("failed to unmap range [%lx-%lx]: %d\n",
			       range->start, range->end, ret);

		list_del(&range->list);
		kfree(range);
	}

	ctx->clean_mm = NULL;

out_unlock:
	mutex_unlock(&ctx->lock);
	return err;
}

/**
 * Abort the clean process of the current process.
 *
 * @param ctx: the context of the memory
 * @return 0 on success, otherwise an error code
 */
static int flux_clean_mm_abort(struct flux_mm_ctx *ctx)
{
	struct mm_struct *mm = current->mm;
	struct flux_mm_range *range, *tmp;
	int err = 0;

	if (!mm)
		return -EINVAL;

	mutex_lock(&ctx->lock);

	if (ctx->clean_mm != mm) {
		err = -EINVAL;
		goto out_unlock;
	}

	list_for_each_entry_safe(range, tmp, &ctx->clean_maps, list) {
		list_del(&range->list);
		kfree(range);
	}

	ctx->clean_mm = NULL;

out_unlock:
	mutex_unlock(&ctx->lock);
	return err;
}

int flux_clean_mm(struct flux_mm_ctx *ctx, int cmd)
{
	switch (cmd) {
	case FLUX_CLEAN_MM_PRE:
		return flux_clean_mm_pre(ctx);
	case FLUX_CLEAN_MM_POST:
		return flux_clean_mm_post(ctx);
	case FLUX_CLEAN_MM_ABORT:
		return flux_clean_mm_abort(ctx);
	default:
		return -EINVAL;
	}
}
