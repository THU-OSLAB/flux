/* Host mmap/brk/exec hooks and process-cache lifecycle. */

#define pr_fmt(fmt) "flux_mm: " fmt

#include <linux/shmem_fs.h>

#include "internal.h"

static struct flux_hook_group execve_hook_group;

struct kmem_cache *flux_proc_cache;

struct flux_proc *flux_proc_alloc(gfp_t flags)
{
	if (WARN_ON(!flux_proc_cache))
		return NULL;

	return kmem_cache_zalloc(flux_proc_cache, flags);
}

void flux_proc_free(struct flux_proc *proc)
{
	kmem_cache_free(flux_proc_cache, proc);
}

/*
 * Only the host runtime's bootstrap mappings are made shared. Application
 * file mappings and COW belong to the LibOS Linux MM after END_MAP_SHARED.
 * The anonymous shmem file is owned by this call, then by the resulting VMA;
 * it never needs a pathname or a slot in the process fd table.
 */
static struct file *flux_copy_runtime_file(struct file *file,
                                           unsigned long len,
                                           unsigned long pgoff)
{
	struct file *copy;
	char *buf;
	loff_t rpos, wpos = 0;
	ssize_t bytes, written;
	int err;

	if (!len || len > LLONG_MAX ||
	    pgoff > (LLONG_MAX - len) >> PAGE_SHIFT)
		return ERR_PTR(-EINVAL);
	rpos = (loff_t)pgoff << PAGE_SHIFT;
	copy = shmem_file_setup("flux-runtime", len, VM_NORESERVE);
	if (IS_ERR(copy))
		return copy;
	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto out_file;
	}

	while (wpos < len) {
		bytes = kernel_read(file, buf, min_t(unsigned long,
						   len - wpos, PAGE_SIZE), &rpos);
		if (bytes < 0) {
			err = bytes;
			goto out_buf;
		}
		if (!bytes)
			break;
		written = kernel_write(copy, buf, bytes, &wpos);
		if (written != bytes) {
			err = written < 0 ? written : -EIO;
			goto out_buf;
		}
	}
	kfree(buf);
	return copy;

out_buf:
	kfree(buf);
out_file:
	fput(copy);
	return ERR_PTR(err);
}

static unsigned long (*orig_vm_mmap_pgoff)(struct file *, unsigned long,
					  unsigned long, unsigned long,
					  unsigned long, unsigned long);

static unsigned long hook_vm_mmap_pgoff(struct file *file, unsigned long addr,
				       unsigned long len, unsigned long prot,
				       unsigned long flags, unsigned long pgoff)
{
	struct file *copy = NULL;
	unsigned long ret;

	/* Driver-owned reservations retain their own mmap contract. */
	if (!test_thread_flag(TIF_HOOK_ACTIVE) ||
	    (file && file->f_op->owner == THIS_MODULE) ||
	    (flags & MAP_TYPE) != MAP_PRIVATE ||
	    (prot != PROT_NONE && !(prot & PROT_WRITE)))
		return orig_vm_mmap_pgoff(file, addr, len, prot, flags, pgoff);

	if (file) {
		/* Preserve native rejection of unreadable file mappings. */
		if (!(file->f_mode & FMODE_READ))
			return orig_vm_mmap_pgoff(file, addr, len, prot, flags,
						  pgoff);
		copy = flux_copy_runtime_file(file, len, pgoff);
		if (IS_ERR(copy))
			return PTR_ERR(copy);
		file = copy;
		pgoff = 0;
	}
	flags = (flags & ~MAP_PRIVATE) | MAP_SHARED;
	ret = orig_vm_mmap_pgoff(file, addr, len, prot, flags, pgoff);
	if (copy)
		fput(copy);
	return ret;
}

static asmlinkage long (*orig_sys_brk)(struct pt_regs *regs);
static long hook_sys_brk(struct pt_regs *regs)
{
	unsigned long old_brk, new_brk, addr, requested_brk;

	if (!test_thread_flag(TIF_HOOK_ACTIVE))
		return orig_sys_brk(regs);

	requested_brk = regs->di;
	old_brk = current->mm->brk;
	new_brk = orig_sys_brk(regs);

	pr_debug("brk(addr=0x%lx)=0x%lx\n", regs->di, new_brk);

	if (!requested_brk || new_brk <= old_brk)
		return new_brk;

	/* Remap the brk range as shared memory. */
	if (new_brk > old_brk) {
		addr = vm_mmap(NULL, old_brk, new_brk - old_brk,
			       PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, 0);
		if (IS_ERR_VALUE(addr) || addr != old_brk) {
			pr_err("failed to remap brk with MAP_SHARED\n");
			regs->di = old_brk;
			orig_sys_brk(regs);
			regs->di = requested_brk;
			return old_brk;
		}
	}

	return new_brk;
}

static struct flux_ftrace_hook mmap_hooks[] = {
	FLUX_HOOK("vm_mmap_pgoff", hook_vm_mmap_pgoff,
		  &orig_vm_mmap_pgoff),
	FLUX_HOOK("__x64_sys_brk", hook_sys_brk, &orig_sys_brk),
};

static struct flux_hook_group mmap_hook_group = {
	.name = "mmap",
	.hooks = mmap_hooks,
	.nr_hooks = ARRAY_SIZE(mmap_hooks),
};

static void flux_mm_hook_set_group(bool active)
{
	struct task_struct *task;

	rcu_read_lock();
	for_each_thread(current, task) {
		if (active)
			set_tsk_thread_flag(task, TIF_HOOK_ACTIVE);
		else
			clear_tsk_thread_flag(task, TIF_HOOK_ACTIVE);
	}
	rcu_read_unlock();
}

int flux_mm_hook_install(void)
{
	flux_mm_hook_set_group(true);
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

	flux_mm_hook_set_group(false);
	return err;
}

int __init flux_mm_hook_init(void)
{
	int err;

	flux_alias_locks_init();

	flux_proc_cache = kmem_cache_create("flux_proc_cache",
					    sizeof(struct flux_proc), 0,
					    SLAB_HWCACHE_ALIGN, NULL);
	if (!flux_proc_cache)
		return -ENOMEM;


	err = flux_mm_resolve_symbols();
	if (err)
		goto out_cache;

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

	err = flux_projection_hook_init();
	if (err) {
		flux_hook_group_remove(&execve_hook_group);
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
	flux_projection_hook_exit();
	flux_hook_group_remove(&execve_hook_group);
	flux_hook_group_remove(&mmap_hook_group);
	if (flux_proc_cache) {
		rcu_barrier();
		kmem_cache_destroy(flux_proc_cache);
		flux_proc_cache = NULL;
	}
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
	FLUX_HOOK("vm_brk_flags", hook_vm_brk_flags, &orig_vm_brk_flags),
};

static struct flux_hook_group execve_hook_group = {
	.name = "execve",
	.hooks = execve_hooks,
	.nr_hooks = ARRAY_SIZE(execve_hooks),
};

void flux_remap_stack(void)
{
	unsigned long stack_top, stack_shift;
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	unsigned long start, end, addr;
	void *buf;

	if (mmap_read_lock_killable(mm))
		return;

	/* Find the stack VMA and remap it as shared memory. */
	stack_top = mm->start_stack;
	vma = find_vma(mm, stack_top - 1);
	if (!vma || !(vma->vm_flags & VM_STACK)) {
		pr_err("failed to find stack VMA for stack top %lx\n",
		       stack_top);
		mmap_read_unlock(mm);
		return;
	}

	pr_debug("remap stack %lx VMA [%lx-%lx] flags=0x%lx\n", stack_top,
		 vma->vm_start, vma->vm_end, vma->vm_flags);

	start = vma->vm_start;
	end = vma->vm_end;
	stack_shift = end - stack_top;
	mmap_read_unlock(mm);

	buf = kmalloc(stack_shift, GFP_KERNEL);
	if (!buf) {
		pr_err("failed to allocate buffer\n");
		return;
	}

	/* Save the stack contents before replacing the mapping. */
	if (copy_from_user(buf, (void __user *)stack_top, stack_shift)) {
		pr_err("failed to copy stack to buffer\n");
		goto out_free;
	}

	/* Replace the stack range with a shared anonymous mapping. */
	addr = vm_mmap(NULL, start, end - start, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, 0);
	if (IS_ERR_VALUE(addr) || addr != start) {
		pr_err("failed to remap stack with MAP_SHARED %lx\n", addr);
		goto out_free;
	}

	/* Restore the stack contents into the new mapping. */
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
