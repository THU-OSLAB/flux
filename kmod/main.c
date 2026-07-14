#define pr_fmt(fmt) "flux: " fmt

#include <linux/mman.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/hugetlb.h>
#include <linux/ptrace.h>
#include <asm/syscall.h>
#include <asm/pgtable_types.h>
#include <asm/apic.h>

#include "dev.h"
#include "hook.h"
#include "mm.h"
#include "mpk.h"

/* per-cpu data to coordinate context switching and signal delivery */
DEFINE_PER_CPU(struct flux_percpu, flux_percpu);

/* shared memory region between kernel and user space */
__read_mostly struct flux_shm *flux_shm;

struct flux_dev {
	const char *name;
	int init;
	int minor;
	struct cdev cdev;
	struct file_operations fops;
};

static dev_t flux_dev_base;
static int flux_dev_region_init;

static long flux_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case FLUX_DEV_IO_START_MAP_SHARED:
		return flux_mm_hook_install();
	case FLUX_DEV_IO_END_MAP_SHARED:
		return flux_mm_hook_remove();
	case FLUX_DEV_IO_EXECVE:
		return flux_execve(arg);
	case FLUX_DEV_IO_DUMP_VMAS:
		flux_dump_vmas();
		return 0;
	default:
		pr_err("unknown ioctl command: %u\n", cmd);
		return -EINVAL; // Not a typewriter (invalid command)
	}
	return 0;
}

static int flux_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int err;
	unsigned long pfn;
	size_t shm_size = sizeof(struct flux_shm) +
			  nr_cpu_ids * sizeof(struct flux_shm_percpu);

	if (vma->vm_end - vma->vm_start < shm_size)
		return -EINVAL;

	pfn = virt_to_phys((void __force *)flux_shm) >> PAGE_SHIFT;

	err = remap_pfn_range(vma, vma->vm_start, pfn, shm_size,
			      vma->vm_page_prot);

	pr_info("mmap: vma [%lx-%lx], size %lu, pfn %lx, err %d\n",
		vma->vm_start, vma->vm_end, shm_size, pfn, err);

	return err;
}

static long flux_uintr_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case FLUX_DEV_IO_UINTR_SETUP:
		return uintr_setup_percpu(filp, arg);
	default:
		pr_err("unknown uintr ioctl command: %u\n", cmd);
		return -EINVAL;
	}
	return 0;
}

static int flux_uintr_open(struct inode *inode, struct file *filp)
{
	pr_info("uintr device opened %lx\n", (unsigned long)filp);
	return 0;
}

static int flux_uintr_release(struct inode *inode, struct file *filp)
{
	pr_info("uintr device released %lx\n", (unsigned long)filp);
	uintr_file_release(filp);
	return 0;
}

static long flux_mm_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct flux_mm_ctx *ctx = (struct flux_mm_ctx *)filp->private_data;
	long err = 0;

	if (!ctx || !kref_get_unless_zero(&ctx->refcount))
		return -EINVAL;

	switch (cmd) {
	case FLUX_DEV_IO_COPY_MM:
		err = flux_copy_mm(ctx, (int __user *)arg);
		break;
	case FLUX_DEV_IO_RELEASE_MM:
		err = flux_release_mm(ctx, arg);
		break;
	case FLUX_DEV_IO_SWITCH_MM:
		err = flux_switch_mm(ctx, arg >> 32, arg);
		break;
	case FLUX_DEV_IO_CLEAN_MM:
		err = flux_clean_mm(ctx, arg);
		break;
	case FLUX_DEV_IO_ENABLE_MPK:
		err = flux_mpk_enable(ctx);
		break;
	case FLUX_DEV_IO_VALIDATE_APP_RANGE:
		err = flux_mpk_validate_app_range(ctx, arg);
		break;
	default:
		pr_err("unknown mm ioctl command: %u\n", cmd);
		err = -EINVAL;
		break;
	}

	kref_put(&ctx->refcount, flux_mm_ctx_release);
	return err;
}

static int flux_mm_open(struct inode *inode, struct file *filp)
{
	int err;

	pr_info("mm device opened %lx\n", (unsigned long)filp);
	err = flux_mm_setup_ctx(filp);
	if (err)
		pr_err("failed to setup mm context: %d\n", err);

	return err;
}

static int flux_mm_release(struct inode *inode, struct file *filp)
{
	struct flux_mm_ctx *ctx = (struct flux_mm_ctx *)filp->private_data;
	if (ctx) {
		filp->private_data = NULL;
		kref_put(&ctx->refcount, flux_mm_ctx_release);
	}
	pr_info("mm device released %lx\n", (unsigned long)filp);
	return 0;
}

static struct flux_dev flux_devs[] = {
	[FLUX_DEV] = {
		.minor = FLUX_DEV,
		.name = FLUX_DEV_NAME,
		.fops = {
			.owner = THIS_MODULE,
			.unlocked_ioctl = flux_ioctl,
			.mmap = flux_mmap,
		}
	},
	[FLUX_UINTR_DEV] = {
		.minor = FLUX_UINTR_DEV,
		.name = FLUX_UINTR_DEV_NAME,
		.fops = {
			.owner = THIS_MODULE,
			.unlocked_ioctl = flux_uintr_ioctl,
			.open = flux_uintr_open,
			.release = flux_uintr_release,
		},
	},
	[FLUX_MM_DEV] = {
		.minor = FLUX_MM_DEV,
		.name = FLUX_MM_DEV_NAME,
		.fops = {
			.owner = THIS_MODULE,
			.unlocked_ioctl = flux_mm_ioctl,
			.open = flux_mm_open,
			.release = flux_mm_release,
		},
	},
};

static int flux_dev_init(struct flux_dev *dev)
{
	int err;
	dev_t devno = MKDEV(MAJOR(flux_dev_base), dev->minor);

	cdev_init(&dev->cdev, &dev->fops);
	err = cdev_add(&dev->cdev, devno, 1);
	if (err) {
		pr_err("failed to add cdev for %s (%d:%d): %d\n", dev->name,
		       MAJOR(devno), MINOR(devno), err);
		return err;
	}

	dev->init = 1;

	pr_info("device %s %d:%d initialized at /dev/%s\n", dev->name,
		MAJOR(devno), MINOR(devno), dev->name);

	return 0;
}

static void flux_dev_del(struct flux_dev *dev)
{
	if (!dev->init)
		return;

	cdev_del(&dev->cdev);
	dev->init = 0;

	pr_info("device %s unregistered\n", dev->name);
}

static int __init flux_init(void)
{
	int err, i;
	size_t size;
	unsigned int order;

	pr_info("---- Flux Kernel Module Initializing ----\n");

	err = alloc_chrdev_region(&flux_dev_base, 0, FLUX_NR_DEVS,
				  FLUX_DEV_NAME);
	if (err) {
		pr_err("failed to allocate char dev region: %d\n", err);
		return err;
	}
	flux_dev_region_init = 1;

	for (i = 0; i < FLUX_NR_DEVS; i++) {
		err = flux_dev_init(&flux_devs[i]);
		if (err) {
			pr_err("failed to init device %s\n", flux_devs[i].name);
			goto fail_dev_init;
		}
	}

	size = sizeof(struct flux_shm) +
	       nr_cpu_ids * sizeof(struct flux_shm_percpu);
	if (size > 2 * 1024 * 1024) {
		pr_err("shared memory size %zu exceeds 2MB\n", size);
		err = -ENOMEM;
		goto fail_shm;
	}
	order = get_order(size);

	// We need to alloc contiguous pages for the shared memory region.
	flux_shm = (struct flux_shm *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
						       order);
	if (!flux_shm) {
		pr_err("failed to allocate shared memory\n");
		err = -ENOMEM;
		goto fail_shm;
	}

	flux_shm->nr_cpus = nr_cpu_ids;
	pr_info("number of CPUs: %d\n", nr_cpu_ids);
	pr_info("shared memory allocated at %llx\n", (u64)flux_shm);

	err = uintr_init();
	if (err) {
		pr_err("failed to initialize UINTR: %d\n", err);
		goto fail_uintr;
	}

	err = flux_hook_init();
	if (err) {
		pr_err("failed to initialize hook support: %d\n", err);
		goto fail_filter;
	}

	err = flux_mm_hook_init();
	if (err) {
		pr_err("failed to initialize mm hooks: %d\n", err);
		goto fail_filter;
	}

	err = flux_mpk_hook_init();
	if (err) {
		pr_err("failed to initialize MPK syscall filter: %d\n", err);
		goto fail_mpk_filter;
	}

	return 0;
fail_mpk_filter:
	flux_mm_hook_exit();
fail_filter:
	uintr_exit();
fail_uintr:
	free_pages((unsigned long)flux_shm, order);
fail_shm:
fail_dev_init:
	for (i = 0; i < FLUX_NR_DEVS; i++)
		flux_dev_del(&flux_devs[i]);
	if (flux_dev_region_init) {
		unregister_chrdev_region(flux_dev_base, FLUX_NR_DEVS);
		flux_dev_region_init = 0;
	}
	return err;
}

static void __exit flux_exit(void)
{
	int i;
	unsigned int order =
		get_order(sizeof(struct flux_shm) +
			  nr_cpu_ids * sizeof(struct flux_shm_percpu));

	flux_mpk_hook_exit();
	flux_mm_hook_exit();

	uintr_exit();

	free_pages((unsigned long)flux_shm, order);

	for (i = 0; i < FLUX_NR_DEVS; i++)
		flux_dev_del(&flux_devs[i]);

	if (flux_dev_region_init) {
		unregister_chrdev_region(flux_dev_base, FLUX_NR_DEVS);
		flux_dev_region_init = 0;
	}

	pr_info("---- Flux Kernel Module Exited ----\n");
}

module_init(flux_init);
module_exit(flux_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaifu Tian");
MODULE_DESCRIPTION("Flux Kernel Module");
MODULE_VERSION("0.01");
