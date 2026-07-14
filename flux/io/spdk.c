#define FLUX_FMT "spdk: "

#include <numa.h>
#include <flux.h>

#include "spdk.h"

#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/queue.h>

#define FLUX_SPDK_CTRL_DEV_PATH "/dev/spdk-control"
#define FLUX_SPDK_CTRL_SYSFS_DEV_PATH "/sys/class/misc/spdk-control/dev"

static int flux_spdk_build_env_context(char *buf, size_t buflen);
static int g_flux_spdk_host_node = SPDK_ENV_NUMA_ID_ANY;

struct flux_spdk_fixed_mapping {
	void *fixed_addr;
	void *backing_addr;
	size_t size;
	STAILQ_ENTRY(flux_spdk_fixed_mapping) link;
};

static pthread_mutex_t g_flux_spdk_fixed_mappings_lock = PTHREAD_MUTEX_INITIALIZER;
static STAILQ_HEAD(, flux_spdk_fixed_mapping) g_flux_spdk_fixed_mappings =
	STAILQ_HEAD_INITIALIZER(g_flux_spdk_fixed_mappings);

static int __spdk_nvme_cpl_is_error(const struct spdk_nvme_cpl *cpl)
{
	return spdk_nvme_cpl_is_error(cpl);
}

struct flux_spdk_dev *spdk_dev;

struct flux_spdk flux_spdk;

static int flux_spdk_get_device_numa_node(const char *bdf, int *node_out);

static int flux_spdk_get_node_first_cpu(int node, int *cpu_out)
{
	struct bitmask *cpumask;
	int cpu;
	int rc = -FLUX_EINVAL;

	cpumask = numa_allocate_cpumask();
	if (!cpumask)
		return -FLUX_ENOMEM;

	if (numa_node_to_cpus(node, cpumask) != 0)
		goto out;

	for (cpu = 0; cpu < (int)cpumask->size; cpu++) {
		if (!numa_bitmask_isbitset(cpumask, cpu))
			continue;

		*cpu_out = cpu;
		rc = 0;
		break;
	}

out:
	numa_free_cpumask(cpumask);
	return rc;
}

static void flux_spdk_context_cleanup(struct flux_spdk_context *ctx)
{
	struct flux_spdk_ns_entry *ns_entry, *ns_next;
	struct flux_spdk_ctrlr_entry *ctrlr_entry, *ctrlr_next;
	unsigned int i;

	assert(ctx != NULL);

	ns_entry = ctx->ns_head;
	while (ns_entry) {
		ns_next = ns_entry->next;
		if (ns_entry->qpairs) {
			for (i = 0; i < ns_entry->qpairs_num; i++) {
				if (ns_entry->qpairs[i]) {
					spdk_nvme_ctrlr_free_io_qpair(
						ns_entry->qpairs[i]);
				}
			}
			free(ns_entry->qpairs);
		}
		free(ns_entry);
		ns_entry = ns_next;
	}

	ctrlr_entry = ctx->ctrlr_head;
	while (ctrlr_entry) {
		ctrlr_next = ctrlr_entry->next;
		spdk_nvme_detach(ctrlr_entry->ctrlr);
		free(ctrlr_entry);
		ctrlr_entry = ctrlr_next;
	}
}

static bool probe_cb(void *ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr_opts *opts)
{
	int i;
	bool found = false;

	for (i = 0; i < run_cfg->spdk_dev_num; i++) {
		if (run_cfg->spdk_bdf[i] == NULL)
			continue;
		if (strcmp(trid->traddr, run_cfg->spdk_bdf[i]) == 0) {
			found = true;
			break;
		}
	}
	if (!found)
		return false;

	FLUX_LOG(FLUX_LOG_INFO, "Attaching to %s\n", trid->traddr);

	/* 
     * Set io_queue_size to UINT16_MAX, NVMe driver
     * will then reduce this to MQES to maximize
     * the io_queue_size as much as possible.
     */
	opts->io_queue_size = UINT16_MAX;

	return true;
}

static int register_ns(struct flux_spdk_context *ctx,
		       struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct flux_spdk_ns_entry *entry;
	struct spdk_nvme_io_qpair_opts opts;

	if (!spdk_nvme_ns_is_active(ns))
		return 0;

	entry = calloc(1, sizeof(struct flux_spdk_ns_entry));
	if (!entry)
		return -FLUX_ENOMEM;

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = ctx->ns_head;
	ctx->ns_head = entry;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));

	FLUX_LOG(FLUX_LOG_INFO, "Namespace ID: %d size: %juGB\n",
		spdk_nvme_ns_get_id(ns),
		spdk_nvme_ns_get_size(ns) / 1000000000);
	return 0;
}

static void attach_cb(void *_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *opts)
{
	struct flux_spdk_ctrlr_entry *entry;
	struct flux_spdk_context *ctx = (struct flux_spdk_context *)_ctx;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_ns *ns;
	int num_ns, nsid, rc;

	ctx->attach_error = 0;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	entry = malloc(sizeof(struct flux_spdk_ctrlr_entry));
	if (!entry) {
		ctx->attach_error = -FLUX_ENOMEM;
		FLUX_LOG(FLUX_LOG_INFO, "spdk_ctrlr_entry malloc failed");
		return;
	}

	FLUX_LOG(FLUX_LOG_INFO, "Attached to %s\n", trid->traddr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)",
		 cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	entry->next = ctx->ctrlr_head;
	ctx->ctrlr_head = entry;

	/*
     * Each controller has one or more namespaces.  An NVMe namespace is
     * basically equivalent to a SCSI LUN.  The controller's IDENTIFY data tells
     * us how many namespaces exist on the controller.  For Intel(R) P3X00
     * controllers, it will just be one namespace.
     *
     * Note that in NVMe, namespace IDs start at 1, not 0.
     */
	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	FLUX_LOG(FLUX_LOG_INFO, "Using controller %s with %d namespaces.\n",
		entry->name, num_ns);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (!ns)
			continue;

		if ((rc = register_ns(ctx, ctrlr, ns)) < 0) {
			ctx->attach_error = rc;
			return;
		}
	}
}

static inline int flux_get_qpairs(struct flux_env *env)
{
	return env->nr_cpus;
}

static int register_qpairs(struct flux_spdk_context *ctx)
{
	int i, nqpairs = flux_get_qpairs(&flux_env);
	struct flux_spdk_ns_entry *ns_entry = ctx->ns_head;

	while (ns_entry != NULL) {
		/*
         * Allocate an I/O qpair that we can use to submit read/write requests
         *  to namespaces on the controller.  NVMe controllers typically support
         *  many qpairs per controller.  Any I/O qpair allocated for a
         * controller can submit I/O to any namespace on that controller.
         *
         * The SPDK NVMe driver provides no synchronization for qpair accesses -
         *  the application must ensure only a single thread submits I/O to a
         *  qpair, and that same thread must also check for completions on that
         *  qpair.  This enables extremely efficient I/O processing by making
         * all I/O operations completely lockless.
         */
		ns_entry->qpairs =
			calloc(nqpairs, sizeof(struct spdk_nvme_qpair *));
		if (!ns_entry->qpairs) {
			FLUX_LOG(FLUX_LOG_ERR,
				"spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return -FLUX_ENOMEM;
		}

		ns_entry->qpairs_num = nqpairs;
		for (i = 0; i < nqpairs; i++) {
			ns_entry->qpairs[i] = spdk_nvme_ctrlr_alloc_io_qpair(
				ns_entry->ctrlr, NULL, 0);
			if (!ns_entry->qpairs[i]) {
				FLUX_LOG(FLUX_LOG_ERR,
					"spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
				return -FLUX_ENOMEM;
			}
		}

		ns_entry = ns_entry->next;
	}

	return 0;
}

static int flux_spdk_context_init(struct flux_spdk_context *ctx)
{
	int rc;
	int node;
	int host_cpu;
	struct spdk_env_opts opts;
	char env_context[256];
	char dpdk_core_mask[32];

	/*
	* SPDK relies on an abstraction around the local environment
	* named env that handles memory allocation and PCI device operations.
	* This library must be initialized first.
	*
	*/
	spdk_env_opts_init(&opts);

	spdk_log_set_print_level(SPDK_LOG_DEBUG);

	opts.name = "./flux";
	opts.shm_id = 0;
	opts.mem_channel = 1;
	opts.opts_size = sizeof(opts);
	rc = flux_spdk_get_device_numa_node(run_cfg->spdk_bdf[0], &node);
	if (rc < 0)
		return rc;
	rc = flux_spdk_get_node_first_cpu(node, &host_cpu);
	if (rc < 0)
		return rc;
	snprintf(dpdk_core_mask, sizeof(dpdk_core_mask), "[%d]", host_cpu);
	opts.core_mask = dpdk_core_mask;
	opts.main_core = host_cpu;
	rc = flux_spdk_build_env_context(env_context, sizeof(env_context));
	if (rc < 0)
		return rc;
	opts.env_context = env_context;

	/*
	 * Force the PCIe transport object into the final static link so its
	 * constructor-based transport registration runs before probe.
	 */
	spdk_nvme_pcie_set_hotplug_filter(NULL);
	rc = spdk_env_init(&opts);
	if (rc) {
		FLUX_LOG(FLUX_LOG_ERR, "spdk_env_init() failed: %s\n",
			flux_strerror(rc));
		return rc;
	}

	ctx->attach_error = -FLUX_ENODEV;
	rc = spdk_nvme_probe(NULL, ctx, probe_cb, attach_cb, NULL);
	if (rc) {
		FLUX_LOG(FLUX_LOG_ERR, "spdk_nvme_probe() failed: %s\n",
			flux_strerror(rc));
		goto out_cleanup_context;
	}

	if (ctx->attach_error) {
		FLUX_LOG(FLUX_LOG_ERR, "attach_cb() failed: %s\n",
			flux_strerror(ctx->attach_error));
		rc = ctx->attach_error;
		goto out_cleanup_context;
	}

	rc = register_qpairs(ctx);
	if (rc) {
		FLUX_LOG(FLUX_LOG_ERR, "register_qpairs() failed: %s\n",
			flux_strerror(rc));
		goto out_cleanup_context;
	}

	return 0;

out_cleanup_context:
	flux_spdk_context_cleanup(ctx);
	return rc;
}

/* Parse the contents of /proc/meminfo (in buf), return value of "name"
 * (example: MemTotal) */
static long get_entry(const char *name, const char *buf)
{
	char *hit = strstr(buf, name);
	if (hit == NULL) {
		return -1;
	}

	errno = 0;
	long val = strtol(hit + strlen(name), NULL, 10);
	if (errno != 0) {
		FLUX_LOG(FLUX_LOG_ERR, "strtol() failed");
		return -1;
	}
	return val;
}

int parse_hugetlb_size(size_t *hugetlb)
{
	char buf[4096];
	size_t size;
	int r = 0;
	int fd = 0;

	r = open("/proc/meminfo", O_RDONLY);
	if (r < 0)
		goto cleanup;
	fd = r;

	r = read(fd, buf, sizeof(buf));
	if (r < 0)
		goto cleanup;

	size = get_entry("Hugetlb:", buf);
	if (!size)
		goto cleanup;

	*hugetlb = size * 1024;

	return 0;

cleanup:
	if (fd)
		close(fd);
	return r;
}

static void flux_spdk_free_dma_memory(struct flux_spdk_dma_memory *dma)
{
	if (dma->data_pool)
		spdk_mempool_free(dma->data_pool);
}

static int flux_spdk_alloc_dma_memory(struct flux_spdk_dev *dev)
{
	struct flux_spdk_context *ctx = &dev->ctx;
	struct flux_spdk_dma_memory *dma = &dev->dma;
	struct flux_spdk_ctrlr_entry *ctrlr_entry;
	struct spdk_nvme_io_qpair_opts opts;
	size_t size = 0, huge;

	/* get total size of huge pages */
	parse_hugetlb_size(&huge);
	if (huge < SPDK_DATA_POOL_ELEM_SIZE * size) {
		FLUX_LOG(FLUX_LOG_INFO, "not enough huge pages\n");
		goto out;
	}

	/* get max queue depth */
	ctrlr_entry = ctx->ctrlr_head;
	while (ctrlr_entry) {
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr_entry->ctrlr,
							  &opts, sizeof(opts));
		if (opts.io_queue_size > size)
			size = opts.io_queue_size;
		ctrlr_entry = ctrlr_entry->next;
	}

	dma->data_pool_size = size - 1;
	if (!flux_env.spdk_zero_copy) {
		dma->data_pool = spdk_mempool_create("flux_dma_pool",
						     dma->data_pool_size,
						     SPDK_DATA_POOL_ELEM_SIZE,
						     0, SPDK_ENV_SOCKET_ID_ANY);
		if (!dma->data_pool) {
			FLUX_LOG(FLUX_LOG_INFO, "spdk_mempool_create() failed\n");
			goto out;
		}
	}

	return 0;
out:
	return -FLUX_ENOMEM;
}

static void flux_host_spdk_init(void)
{
	flux_spdk.ops.cpl_is_error = __spdk_nvme_cpl_is_error;
	flux_spdk.ops.ns_get_size = spdk_nvme_ns_get_size;
	flux_spdk.ops.ns_get_sector_size = spdk_nvme_ns_get_sector_size;
	flux_spdk.ops.ns_cmd_write = spdk_nvme_ns_cmd_write;
	flux_spdk.ops.ns_cmd_writev = spdk_nvme_ns_cmd_writev;
	flux_spdk.ops.ns_cmd_read = spdk_nvme_ns_cmd_read;
	flux_spdk.ops.ns_cmd_readv = spdk_nvme_ns_cmd_readv;
	flux_spdk.ops.mempool_get = spdk_mempool_get;
	flux_spdk.ops.mempool_put = spdk_mempool_put;
	flux_spdk.ops.qpair_process_completions =
		spdk_nvme_qpair_process_completions;

	flux_spdk.zero_copy = flux_env.spdk_zero_copy;

	flux_spdk.dma_mempool = spdk_dev->dma.data_pool;
	flux_spdk.dma_mempool_size = spdk_dev->dma.data_pool_size;
}

int flux_spdk_init(void)
{
	int rc = 0;

	if (!run_cfg->spdk_dev_num)
		return 0;

#if SPDK_VERSION_MAJOR < 23
	FLUX_LOG(FLUX_LOG_INFO, "SPDK version 23 or later is required\n");
	return -FLUX_EINVAL;
#endif

	spdk_dev = calloc(1, sizeof(struct flux_spdk_dev));
	if (!spdk_dev) {
		FLUX_LOG(FLUX_LOG_INFO, "failed to allocate spdk_dev\n");
		return -FLUX_ENOMEM;
	}

	rc = flux_spdk_context_init(&spdk_dev->ctx);
	if (rc) {
		FLUX_LOG(FLUX_LOG_INFO, "failed to initialize spdk context\n");
		goto out_free_dev;
	}

	rc = flux_spdk_alloc_dma_memory(spdk_dev);
	if (rc) {
		FLUX_LOG(FLUX_LOG_INFO, "failed to allocate spdk dma memory\n");
		goto out_free_ctx;
	}

	flux_host_spdk_init();

	return rc;

out_free_ctx:
	flux_spdk_context_cleanup(&spdk_dev->ctx);
out_free_dev:
	free(spdk_dev);
	spdk_dev = NULL;
	return rc;
}

void flux_spdk_fini(void)
{
	if (!run_cfg->spdk_dev_num)
		return;

	if (spdk_dev) {
		flux_spdk_free_dma_memory(&spdk_dev->dma);
		flux_spdk_context_cleanup(&spdk_dev->ctx);
		free(spdk_dev);
		spdk_dev = NULL;
	}
	flux_spdk.dma_mempool = NULL;
	flux_spdk.dma_mempool_size = 0;
	g_flux_spdk_host_node = SPDK_ENV_NUMA_ID_ANY;
}

static int flux_mount_blockdev(char *dev_name, char *mnt_point, char *fs_type,
			      int flags, char *data)
{
	int err;

	err = flux_sys_access("/mnt", S_IRWXO);
	if (err < 0) {
		if (err == -FLUX_ENOENT)
			err = flux_sys_mkdir("/mnt", 0700);
		if (err < 0)
			goto fail;
	}

	err = flux_sys_mkdir(mnt_point, 0700);
	if (err < 0)
		goto fail;

	err = flux_sys_mount(dev_name, mnt_point, fs_type, flags, data);
	if (err < 0) {
		flux_sys_rmdir(mnt_point);
		goto fail;
	}

fail:
	return err;
}

static int flux_spdk_mknod_ctrl_dev(void)
{
	int ret;
	int fd;
	unsigned int major;
	unsigned int minor;
	char devno[32];

	fd = flux_sys_open(FLUX_SPDK_CTRL_SYSFS_DEV_PATH, FLUX_O_RDONLY, 0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open %s: %s\n",
			 FLUX_SPDK_CTRL_SYSFS_DEV_PATH, flux_strerror(fd));
		return fd;
	}

	ret = flux_sys_read(fd, devno, sizeof(devno) - 1);
	flux_sys_close(fd);
	if (ret <= 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to read %s: %s\n",
			 FLUX_SPDK_CTRL_SYSFS_DEV_PATH,
			 ret < 0 ? flux_strerror(ret) : "empty file");
		return ret < 0 ? ret : -FLUX_EINVAL;
	}
	devno[ret] = '\0';

	if (sscanf(devno, "%u:%u", &major, &minor) != 2) {
		FLUX_LOG(FLUX_LOG_ERR, "invalid device number format: %s\n",
			 devno);
		return -FLUX_EINVAL;
	}

	ret = flux_sys_mknod(FLUX_SPDK_CTRL_DEV_PATH, FLUX_S_IFCHR | 0600,
			     FLUX_MKDEV(major, minor));
	if (ret == -FLUX_EEXIST) {
		ret = flux_sys_unlink(FLUX_SPDK_CTRL_DEV_PATH);
		if (ret < 0 && ret != -FLUX_ENOENT) {
			FLUX_LOG(FLUX_LOG_ERR, "failed to unlink %s: %s\n",
				 FLUX_SPDK_CTRL_DEV_PATH, flux_strerror(ret));
			return ret;
		}

		ret = flux_sys_mknod(FLUX_SPDK_CTRL_DEV_PATH,
				     FLUX_S_IFCHR | 0600,
				     FLUX_MKDEV(major, minor));
	}

	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to mknod %s (%u:%u): %s\n",
			 FLUX_SPDK_CTRL_DEV_PATH, major, minor,
			 flux_strerror(ret));
		return ret;
	}

	return 0;
}

int flux_spdk_register_dev(struct flux_spdk_dev *dev)
{
	int ret, dev_id;
	char mnt_path[FLUX_PATH_MAX], dev_path[FLUX_PATH_MAX];
	struct flux_spdk_ns_entry *ns_entry;

	if (!run_cfg->spdk_dev_num)
		return 0;

	dev->ioctl_fd = flux_sys_open(FLUX_SPDK_CTRL_DEV_PATH, O_RDWR, 0);
	if (dev->ioctl_fd < 0) {
		ret = flux_spdk_mknod_ctrl_dev();
		if (ret < 0)
			return ret;

		dev->ioctl_fd = flux_sys_open(FLUX_SPDK_CTRL_DEV_PATH, O_RDWR, 0);
		if (dev->ioctl_fd < 0) {
			FLUX_LOG(FLUX_LOG_INFO, "failed to open %s after mknod: %s\n",
				 FLUX_SPDK_CTRL_DEV_PATH,
				 flux_strerror(dev->ioctl_fd));
			return dev->ioctl_fd;
		}
	}

	ns_entry = dev->ctx.ns_head;
	while (ns_entry) {
		ret = flux_sys_ioctl(dev->ioctl_fd, FLUX_SPDK_IOCTL_ADD,
				    (unsigned long)ns_entry);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_INFO, "failed to ioctl: %s\n",
				flux_strerror(ret));
			return ret;
		}

		dev_id = ret;

		/* mount spdk block device */
		snprintf(dev_path, FLUX_PATH_MAX, "/dev/spdk%d", dev_id);
		snprintf(mnt_path, FLUX_PATH_MAX, "/mnt/spdk%d", dev_id);
		ret = flux_mount_blockdev(dev_path, mnt_path, run_cfg->spdk_fs_type,
					 0, NULL);
		if (ret < 0) {
			FLUX_LOG(FLUX_LOG_INFO, "failed to mount %s: %s\n",
				dev_path, flux_strerror(ret));
			return ret;
		}

		ns_entry = ns_entry->next;
	}

	flux_sys_close(dev->ioctl_fd);

	return 0;
}

static int flux_spdk_read_long_file(const char *path, long *value)
{
	FILE *f;
	long v;

	f = fopen(path, "r");
	if (!f)
		return -FLUX_EIO;

	if (fscanf(f, "%ld", &v) != 1) {
		fclose(f);
		return -FLUX_EIO;
	}

	fclose(f);
	*value = v;
	return 0;
}

static int flux_spdk_get_device_numa_node(const char *bdf, int *node_out)
{
	char path[PATH_MAX];
	long node;
	int rc;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/numa_node", bdf);
	rc = flux_spdk_read_long_file(path, &node);
	if (rc < 0 || node < 0)
		return -FLUX_EINVAL;

	*node_out = (int)node;
	return 0;
}

static int flux_spdk_host_node_get(void)
{
	int rc;

	if (g_flux_spdk_host_node != SPDK_ENV_NUMA_ID_ANY)
		return g_flux_spdk_host_node;

	if (!run_cfg || !run_cfg->spdk_dev_num || !run_cfg->spdk_bdf ||
	    !run_cfg->spdk_bdf[0])
		return SPDK_ENV_NUMA_ID_ANY;

	rc = flux_spdk_get_device_numa_node(run_cfg->spdk_bdf[0],
					    &g_flux_spdk_host_node);
	if (rc < 0)
		return SPDK_ENV_NUMA_ID_ANY;

	return g_flux_spdk_host_node;
}

static int flux_spdk_get_hugepage_info(long *page_kb_out, long *page_num_out)
{
	FILE *f;
	char line[256];
	long page_kb = 0;
	long page_num = 0;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -FLUX_EIO;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "Hugepagesize: %ld kB", &page_kb) == 1)
			continue;
		if (sscanf(line, "Hugetlb: %ld kB", &page_num) == 1)
			continue;
	}

	fclose(f);

	if (page_kb <= 0)
		return -FLUX_EINVAL;

	*page_kb_out = page_kb;
	*page_num_out = page_num / page_kb;
	return 0;
}

static int flux_spdk_get_node_hugepages_mb(int node, long *mb_out)
{
	char path[PATH_MAX];
	long page_kb;
	long page_num;
	long node_page_num;
	int rc;

	rc = flux_spdk_get_hugepage_info(&page_kb, &page_num);
	if (rc < 0)
		return rc;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/hugepages/hugepages-%ldkB/nr_hugepages",
		 node, page_kb);
	rc = flux_spdk_read_long_file(path, &node_page_num);
	if (rc < 0 || node_page_num <= 0)
		return -FLUX_EINVAL;

	*mb_out = (node_page_num * page_kb) / 1024;
	return 0;
}

static int flux_spdk_build_env_context(char *buf, size_t buflen)
{
	long node_mb;
	int node;
	int sockets;
	int i;
	int len = 0;
	int rc;

	if (!run_cfg || !run_cfg->spdk_dev_num || !run_cfg->spdk_bdf ||
	    !run_cfg->spdk_bdf[0])
		return -FLUX_EINVAL;

	rc = flux_spdk_get_device_numa_node(run_cfg->spdk_bdf[0], &node);
	if (rc < 0)
		return rc;

	rc = flux_spdk_get_node_hugepages_mb(node, &node_mb);
	if (rc < 0)
		return rc;

	sockets = numa_max_node() + 1;
	if (sockets <= 0 || node >= sockets)
		return -FLUX_EINVAL;

	len = snprintf(buf, buflen, "--socket-mem=");
	if (len < 0 || (size_t)len >= buflen)
		return -FLUX_ENOMEM;

	for (i = 0; i < sockets; i++) {
		long mb = (i == node) ? node_mb : 0;
		int wrote = snprintf(buf + len, buflen - (size_t)len, "%s%ld",
				     i == 0 ? "" : ",", mb);
		if (wrote < 0 || (size_t)wrote >= buflen - (size_t)len)
			return -FLUX_ENOMEM;
		len += wrote;
	}

	if ((size_t)len >= buflen)
		return -FLUX_ENOMEM;

	snprintf(buf + len, buflen - (size_t)len, " --base-virtaddr=0x0");
	FLUX_LOG(FLUX_LOG_INFO, "spdk env_context: %s\n", buf);
	return 0;
}

static void *
flux_spdk_dma_malloc_fixed(void *hint, size_t size, size_t align, int flags)
{
	struct flux_spdk_fixed_mapping *mapping = NULL;
	void *backing_addr = NULL;
	void *fixed_addr = MAP_FAILED;
	uint64_t offset = 0;
	size_t alloc_size;
	size_t mapped_size = 0;
	int fd = -1;
	int rc;

	alloc_size = align_up(size, PGSIZE_2MB);
	if (align > PGSIZE_2MB)
		alloc_size = align_up(alloc_size, align);

	backing_addr = spdk_dma_zmalloc_socket(alloc_size, align, NULL,
					       flux_spdk_host_node_get());
	if (!backing_addr)
		goto out;

	fd = spdk_mem_get_fd_and_offset(backing_addr, &offset);
	if (fd < 0)
		goto out;

	fixed_addr = mmap(hint, alloc_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_FIXED_NOREPLACE, fd, (off_t)offset);
	if (fixed_addr == MAP_FAILED || fixed_addr != hint) {
		if (fixed_addr != MAP_FAILED && fixed_addr != hint)
			munmap(fixed_addr, alloc_size);
		fixed_addr = hint;
		while (mapped_size < alloc_size) {
			void *chunk_hint = (char *)hint + mapped_size;
			void *backing_chunk = (char *)backing_addr + mapped_size;
			void *chunk_addr;
			uint64_t chunk_offset = 0;
			int chunk_fd;

			chunk_fd = spdk_mem_get_fd_and_offset(backing_chunk,
							      &chunk_offset);
			if (chunk_fd < 0)
				goto out;

			chunk_addr = mmap(chunk_hint, PGSIZE_2MB,
					  PROT_READ | PROT_WRITE,
					  MAP_SHARED | MAP_FIXED_NOREPLACE,
					  chunk_fd, (off_t)chunk_offset);
			if (chunk_addr == MAP_FAILED || chunk_addr != chunk_hint) {
				if (chunk_addr != MAP_FAILED &&
				    chunk_addr != chunk_hint)
					munmap(chunk_addr, PGSIZE_2MB);
				goto out;
			}
			mapped_size += PGSIZE_2MB;
		}
	} else {
		mapped_size = alloc_size;
	}

	rc = spdk_mem_register(fixed_addr, alloc_size);
	if (rc)
		goto out;

	mapping = calloc(1, sizeof(*mapping));
	if (!mapping)
		goto out_unregister;

	mapping->fixed_addr = fixed_addr;
	mapping->backing_addr = backing_addr;
	mapping->size = alloc_size;

	pthread_mutex_lock(&g_flux_spdk_fixed_mappings_lock);
	STAILQ_INSERT_TAIL(&g_flux_spdk_fixed_mappings, mapping, link);
	pthread_mutex_unlock(&g_flux_spdk_fixed_mappings_lock);

	return fixed_addr;

out_unregister:
	spdk_mem_unregister(fixed_addr, alloc_size);
out:
	if (fixed_addr != MAP_FAILED && mapped_size)
		munmap(fixed_addr, mapped_size);
	if (backing_addr)
		spdk_dma_free(backing_addr);
	return NULL;
}

void *flux_spdk_dma_malloc(void *hint, size_t size, size_t align, int flags)
{
	if (hint)
		return flux_spdk_dma_malloc_fixed(hint, size, align, flags);

	return spdk_dma_malloc_socket(size, align, NULL,
				      flux_spdk_host_node_get());
}

void flux_spdk_dma_free(void *addr, size_t size)
{
	struct flux_spdk_fixed_mapping *mapping;
	struct flux_spdk_fixed_mapping *prev = NULL;

	pthread_mutex_lock(&g_flux_spdk_fixed_mappings_lock);
	STAILQ_FOREACH(mapping, &g_flux_spdk_fixed_mappings, link) {
		if (mapping->fixed_addr == addr)
			break;
		prev = mapping;
	}
	if (mapping) {
		if (prev)
			STAILQ_REMOVE_AFTER(&g_flux_spdk_fixed_mappings, prev, link);
		else
			STAILQ_REMOVE_HEAD(&g_flux_spdk_fixed_mappings, link);
	}
	pthread_mutex_unlock(&g_flux_spdk_fixed_mappings_lock);

	if (mapping) {
		spdk_mem_unregister(mapping->fixed_addr, mapping->size);
		munmap(mapping->fixed_addr, mapping->size);
		spdk_dma_free(mapping->backing_addr);
		free(mapping);
		return;
	}

	spdk_dma_free(addr);
}
