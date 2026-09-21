// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fcntl.h>
#include <linux/fs_struct.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mount.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/sched/mm.h>

#ifdef CONFIG_FLUX
#include <asm/unistd.h>
#include <asm/host_dev.h>
#include <asm/syscalls.h>
#endif

#include "internal.h"

/*
 * Logic: we've got two memory sums for each process, "shared", and
 * "non-shared". Shared memory may get counted more than once, for
 * each process that owns it. Non-shared memory is counted
 * accurately.
 */
void task_mem(struct seq_file *m, struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	struct vm_region *region;
	unsigned long bytes = 0, sbytes = 0, slack = 0, size;

	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		bytes += kobjsize(vma);

		region = vma->vm_region;
		if (region) {
			size = kobjsize(region);
			size += region->vm_end - region->vm_start;
		} else {
			size = vma->vm_end - vma->vm_start;
		}

		if (atomic_read(&mm->mm_count) > 1 ||
		    is_nommu_shared_mapping(vma->vm_flags)) {
			sbytes += size;
		} else {
			bytes += size;
			if (region)
				slack = region->vm_end - vma->vm_end;
		}
	}

	if (atomic_read(&mm->mm_count) > 1)
		sbytes += kobjsize(mm);
	else
		bytes += kobjsize(mm);

	if (current->fs && current->fs->users > 1)
		sbytes += kobjsize(current->fs);
	else
		bytes += kobjsize(current->fs);

	if (current->files && atomic_read(&current->files->count) > 1)
		sbytes += kobjsize(current->files);
	else
		bytes += kobjsize(current->files);

	if (current->sighand && refcount_read(&current->sighand->count) > 1)
		sbytes += kobjsize(current->sighand);
	else
		bytes += kobjsize(current->sighand);

	bytes += kobjsize(current); /* includes kernel stack */

	mmap_read_unlock(mm);

	seq_printf(m,
		"Mem:\t%8lu bytes\n"
		"Slack:\t%8lu bytes\n"
		"Shared:\t%8lu bytes\n",
		bytes, slack, sbytes);
}

unsigned long task_vsize(struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	unsigned long vsize = 0;

	mmap_read_lock(mm);
	for_each_vma(vmi, vma)
		vsize += vma->vm_end - vma->vm_start;
	mmap_read_unlock(mm);
	return vsize;
}

unsigned long task_statm(struct mm_struct *mm,
			 unsigned long *shared, unsigned long *text,
			 unsigned long *data, unsigned long *resident)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	struct vm_region *region;
	unsigned long size = kobjsize(mm);

	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		size += kobjsize(vma);
		region = vma->vm_region;
		if (region) {
			size += kobjsize(region);
			size += region->vm_end - region->vm_start;
		}
	}

	*text = (PAGE_ALIGN(mm->end_code) - (mm->start_code & PAGE_MASK))
		>> PAGE_SHIFT;
	*data = (PAGE_ALIGN(mm->start_stack) - (mm->start_data & PAGE_MASK))
		>> PAGE_SHIFT;
	mmap_read_unlock(mm);
	size >>= PAGE_SHIFT;
	size += *text + *data;
	*resident = size;
	return size;
}

/*
 * display a single VMA to a sequenced file
 */
static int nommu_vma_show(struct seq_file *m, struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long ino = 0;
	struct file *file;
	dev_t dev = 0;
	int flags;
	unsigned long long pgoff = 0;

	flags = vma->vm_flags;
	file = vma->vm_file;

	if (file) {
		struct inode *inode = file_inode(vma->vm_file);
		dev = inode->i_sb->s_dev;
		ino = inode->i_ino;
		pgoff = (loff_t)vma->vm_pgoff << PAGE_SHIFT;
	}

	seq_setwidth(m, 25 + sizeof(void *) * 6 - 1);
	seq_printf(m,
		   "%08lx-%08lx %c%c%c%c %08llx %02x:%02x %lu ",
		   vma->vm_start,
		   vma->vm_end,
		   flags & VM_READ ? 'r' : '-',
		   flags & VM_WRITE ? 'w' : '-',
		   flags & VM_EXEC ? 'x' : '-',
		   flags & VM_MAYSHARE ? flags & VM_SHARED ? 'S' : 's' : 'p',
		   pgoff,
		   MAJOR(dev), MINOR(dev), ino);

	if (file) {
		seq_pad(m, ' ');
		seq_file_path(m, file, "");
	} else if (mm && vma_is_initial_stack(vma)) {
		seq_pad(m, ' ');
		seq_puts(m, "[stack]");
	}

	seq_putc(m, '\n');
	return 0;
}

/*
 * display mapping lines for a particular process's /proc/pid/maps
 */
static int show_map(struct seq_file *m, void *_p)
{
	return nommu_vma_show(m, _p);
}

static struct vm_area_struct *proc_get_vma(struct proc_maps_private *priv,
						loff_t *ppos)
{
	struct vm_area_struct *vma = vma_next(&priv->iter);

	if (vma) {
		*ppos = vma->vm_start;
	} else {
		*ppos = -1UL;
	}

	return vma;
}

static void *m_start(struct seq_file *m, loff_t *ppos)
{
	struct proc_maps_private *priv = m->private;
	unsigned long last_addr = *ppos;
	struct mm_struct *mm;

	/* See proc_get_vma(). Zero at the start or after lseek. */
	if (last_addr == -1UL)
		return NULL;

	/* pin the task and mm whilst we play with them */
	priv->task = get_proc_task(priv->inode);
	if (!priv->task)
		return ERR_PTR(-ESRCH);

	mm = priv->mm;
	if (!mm || !mmget_not_zero(mm)) {
		put_task_struct(priv->task);
		priv->task = NULL;
		return NULL;
	}

	if (mmap_read_lock_killable(mm)) {
		mmput(mm);
		put_task_struct(priv->task);
		priv->task = NULL;
		return ERR_PTR(-EINTR);
	}

	vma_iter_init(&priv->iter, mm, last_addr);

	return proc_get_vma(priv, ppos);
}

static void m_stop(struct seq_file *m, void *v)
{
	struct proc_maps_private *priv = m->private;
	struct mm_struct *mm = priv->mm;

	if (!priv->task)
		return;

	mmap_read_unlock(mm);
	mmput(mm);
	put_task_struct(priv->task);
	priv->task = NULL;
}

static void *m_next(struct seq_file *m, void *_p, loff_t *ppos)
{
	return proc_get_vma(m->private, ppos);
}

static const struct seq_operations proc_pid_maps_ops = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_map
};

static int maps_open(struct inode *inode, struct file *file,
		     const struct seq_operations *ops)
{
	struct proc_maps_private *priv;

	priv = __seq_open_private(file, ops, sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	priv->inode = inode;
	priv->mm = proc_mem_open(inode, PTRACE_MODE_READ);
	if (IS_ERR(priv->mm)) {
		int err = PTR_ERR(priv->mm);

		seq_release_private(inode, file);
		return err;
	}

	return 0;
}


static int map_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct proc_maps_private *priv = seq->private;

	if (priv->mm)
		mmdrop(priv->mm);

	return seq_release_private(inode, file);
}

static int pid_maps_open(struct inode *inode, struct file *file)
{
	return maps_open(inode, file, &proc_pid_maps_ops);
}

const struct file_operations proc_pid_maps_operations = {
	.open		= pid_maps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= map_release,
};

#ifdef CONFIG_FLUX

/* Proxy host accounting while exposing only the active guest application's VMAs. */
#define FLUX_SMAPS_LINE_SIZE (PATH_MAX + 512)

static bool flux_smaps_range_is_app(const struct flux_mpk_base_map *maps,
				    unsigned int nr, unsigned long start,
				    unsigned long end)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (start < maps[i].end) {
			if (end > maps[i].start)
				return true;
		}
	}
	return false;
}

static void flux_smaps_process_line(struct seq_file *m, char *line, size_t len,
				    const struct flux_mpk_base_map *maps,
				    unsigned int nr, bool *emit)
{
	unsigned long start, end;

	line[len] = '\0';
	if (sscanf(line, "%lx-%lx", &start, &end) == 2)
		*emit = flux_smaps_range_is_app(maps, nr, start, end);
	if (*emit) {
		seq_write(m, line, len);
		seq_putc(m, '\n');
	}
}

static int flux_smaps_get_app_maps(int proc_key,
				   struct flux_mpk_base_map **maps_out,
				   unsigned int *nr_out)
{
	struct flux_mpk_base_map *maps;
	unsigned int nr = 0, fetched = 0;
	int ret;

	ret = flux_host_dev_get_app_maps(proc_key, NULL, 0, &nr);
	if (ret != -ENOSPC || !nr)
		return ret ? ret : -ENOENT;

	maps = kcalloc(nr, sizeof(*maps), GFP_KERNEL);
	if (!maps)
		return -ENOMEM;

	ret = flux_host_dev_get_app_maps(proc_key, maps, nr, &fetched);
	if (ret || fetched != nr) {
		kfree(maps);
		return ret ? ret : -EAGAIN;
	}

	*maps_out = maps;
	*nr_out = nr;
	return 0;
}

static int flux_smaps_show_proxy(struct seq_file *m)
{
	static const char host_path[] = "/proc/thread-self/smaps";
	struct flux_mpk_base_map *maps = NULL;
	char *read_buf = NULL, *line = NULL;
	unsigned int nr = 0;
	long host_fd, nread;
	size_t line_len = 0;
	bool emit = false;
	int proc_key = (int)(long)m->private;
	long i;
	int ret;

	ret = flux_smaps_get_app_maps(proc_key, &maps, &nr);
	if (ret)
		return ret;

	read_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	line = kmalloc(FLUX_SMAPS_LINE_SIZE, GFP_KERNEL);
	if (!read_buf || !line) {
		ret = -ENOMEM;
		goto out_free;
	}

	host_fd = host_syscall(__NR_openat, AT_FDCWD, host_path,
				O_RDONLY | O_CLOEXEC, 0);
	if (host_fd < 0) {
		ret = host_fd;
		goto out_free;
	}

	for (;;) {
		nread = host_syscall(__NR_read, host_fd, read_buf, PAGE_SIZE);
		if (nread == -EINTR)
			continue;
		if (nread < 0) {
			ret = nread;
			break;
		}
		if (!nread) {
			if (line_len)
				flux_smaps_process_line(m, line, line_len, maps, nr,
							&emit);
			ret = 0;
			break;
		}

		for (i = 0; i < nread; i++) {
			if (read_buf[i] == '\n') {
				flux_smaps_process_line(m, line, line_len, maps, nr,
							&emit);
				line_len = 0;
				continue;
			}
			if (line_len == FLUX_SMAPS_LINE_SIZE - 1) {
				ret = -EOVERFLOW;
				goto out_close;
			}
			line[line_len++] = read_buf[i];
		}
	}

out_close:
	host_syscall(__NR_close, host_fd);
out_free:
	kfree(line);
	kfree(read_buf);
	kfree(maps);
	return ret;
}

static void flux_smaps_put_kb(struct seq_file *m, const char *field, u64 bytes)
{
	seq_printf(m, "%-16s %8llu kB\n", field, bytes >> 10);
}

static int flux_smaps_show_fast(struct seq_file *m)
{
	struct flux_mm_smaps_stats stats = {};
	int proc_key = (int)(long)m->private;
	int ret;

	ret = flux_host_dev_get_app_smaps(proc_key, &stats);
	if (ret)
		return ret;

	seq_printf(m,
		   "%08llx-%08llx ---p 00000000 00:00 0 [flux-app-rollup]\n",
		   stats.start, stats.end);
	flux_smaps_put_kb(m, "Size:", stats.size);
	flux_smaps_put_kb(m, "Rss:", stats.resident);
	flux_smaps_put_kb(m, "Pss:", stats.pss);
	flux_smaps_put_kb(m, "Pss_Dirty:", stats.pss_dirty);
	flux_smaps_put_kb(m, "Pss_Anon:", stats.pss_anon);
	flux_smaps_put_kb(m, "Pss_File:", stats.pss_file);
	flux_smaps_put_kb(m, "Pss_Shmem:", stats.pss_shmem);
	flux_smaps_put_kb(m, "Shared_Clean:", stats.shared_clean);
	flux_smaps_put_kb(m, "Shared_Dirty:", stats.shared_dirty);
	flux_smaps_put_kb(m, "Private_Clean:", stats.private_clean);
	flux_smaps_put_kb(m, "Private_Dirty:", stats.private_dirty);
	flux_smaps_put_kb(m, "Referenced:", stats.referenced);
	flux_smaps_put_kb(m, "Anonymous:", stats.anonymous);
	flux_smaps_put_kb(m, "KSM:", stats.ksm);
	flux_smaps_put_kb(m, "LazyFree:", stats.lazyfree);
	flux_smaps_put_kb(m, "AnonHugePages:", stats.anonymous_thp);
	flux_smaps_put_kb(m, "ShmemPmdMapped:", stats.shmem_thp);
	flux_smaps_put_kb(m, "FilePmdMapped:", stats.file_thp);
	flux_smaps_put_kb(m, "Shared_Hugetlb:", stats.shared_hugetlb);
	flux_smaps_put_kb(m, "Private_Hugetlb:", stats.private_hugetlb);
	flux_smaps_put_kb(m, "Swap:", stats.swap);
	flux_smaps_put_kb(m, "SwapPss:", stats.swap_pss);
	flux_smaps_put_kb(m, "Locked:", stats.locked);
	return 0;
}

static int flux_smaps_show(struct seq_file *m, void *unused)
{
	int ret;

	(void)unused;

	ret = flux_smaps_show_fast(m);
	if (!ret)
		return 0;
	return flux_smaps_show_proxy(m);
}

static int flux_smaps_open(struct inode *inode, struct file *file)
{
	struct task_struct *task;
	struct mm_struct *mm;
	int proc_key = 0, ret = 0;

	task = get_proc_task(inode);
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;
	if (mm != current->mm)
		ret = -EOPNOTSUPP;
	else
		proc_key = mm->context.proc_key;
	mmput(mm);
	if (ret)
		return ret;

	return single_open(file, flux_smaps_show, (void *)(long)proc_key);
}

const struct file_operations proc_pid_smaps_operations = {
	.open		= flux_smaps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#endif /* CONFIG_FLUX */
