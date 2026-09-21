#define pr_fmt(fmt) "execd: " fmt

#include <linux/fs_struct.h>
#include <linux/binfmts.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/idle.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include <asm/signal.h>
#include <asm/console.h>
#include <asm/syscalls.h>
#include <asm/thread_info.h>
#include <asm/x86/ptrace.h>
#include <asm/x86/current.h>
#include <asm/x86/processor.h>
#include <uapi/linux/mman.h>
#include <uapi/asm/mman.h>
#include <uapi/asm/flux_oci.h>

#ifdef CONFIG_FLUX_RUNC

static inline int flux_wait_wifexited(int stat)
{
	return (stat & 0x7f) == 0;
}

static inline int flux_wait_wexitstatus(int stat)
{
	return (stat >> 8) & 0xff;
}

static inline int flux_wait_wifsignaled(int stat)
{
	return ((stat & 0x7f) != 0) && ((stat & 0x7f) != 0x7f);
}

static inline int flux_wait_wtermsig(int stat)
{
	return stat & 0x7f;
}

static atomic_t flux_exec_pending = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(flux_exec_waitq);

struct flux_exec_child_req {
	char *filename;
	char *cwd;
	char **argv;
	char **envp;
	uid_t uid;
	gid_t gid;
	u32 session_id;
	bool has_user;
};

struct flux_exec_dispatch_req {
	struct flux_exec_child_req *req;
	struct flux_exec_slot *slot;
	uint32_t slot_idx;
};

static struct flux_exec_ring *flux_exec_ring(void)
{
	return (struct flux_exec_ring *)(unsigned long)FLUX_EXEC_RING_MAP_ADDR;
}

#define FLUX_RESOURCE_CGROUP "/sys/fs/cgroup/flux-oci"

static int flux_resource_path(char *buf, size_t size, const char *name)
{
	int len = snprintf(buf, size, "%s/%s", FLUX_RESOURCE_CGROUP, name);

	return len < 0 || len >= size ? -ENAMETOOLONG : 0;
}

static int flux_resource_write(const char *name, const char *value)
{
	struct file *file;
	char path[128];
	loff_t pos = 0;
	ssize_t len = strlen(value);
	ssize_t nw;
	int ret;

	ret = flux_resource_path(path, sizeof(path), name);
	if (ret < 0)
		return ret;
	file = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	nw = kernel_write(file, value, len, &pos);
	filp_close(file, NULL);
	return nw == len ? 0 : nw < 0 ? (int)nw : -EIO;
}

static int flux_resource_read(const char *name, char *buf, size_t size)
{
	struct file *file;
	char path[128];
	loff_t pos = 0;
	ssize_t nr;
	int ret;

	if (size < 2)
		return -EINVAL;
	ret = flux_resource_path(path, sizeof(path), name);
	if (ret < 0)
		return ret;
	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	nr = kernel_read(file, buf, size - 1, &pos);
	filp_close(file, NULL);
	if (nr < 0)
		return (int)nr;
	buf[nr] = '\0';
	return 0;
}

static int flux_resource_write_i64(const char *name, s64 value,
				   bool max_for_negative)
{
	char buf[64];

	if (value < 0 && max_for_negative)
		snprintf(buf, sizeof(buf), "max\n");
	else
		snprintf(buf, sizeof(buf), "%lld\n", (long long)value);
	return flux_resource_write(name, buf);
}

static int flux_resource_write_u64(const char *name, u64 value)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)value);
	return flux_resource_write(name, buf);
}

static u64 flux_resource_parse_u64(const char *value)
{
	unsigned long long result = 0;

	while (*value == ' ' || *value == '\t')
		value++;
	if (!strncmp(value, "max", 3))
		return U64_MAX;
	if (kstrtoull(value, 10, &result) < 0)
		return 0;
	return (u64)result;
}

static u64 flux_resource_stat_value(const char *buf, const char *key)
{
	const char *line = buf;
	size_t key_len = strlen(key);

	while (line && *line) {
		const char *next = strchr(line, '\n');
		if (!strncmp(line, key, key_len) &&
		    (line[key_len] == ' ' || line[key_len] == '='))
			return flux_resource_parse_u64(line + key_len + 1);
		line = next ? next + 1 : NULL;
	}
	return 0;
}

static int flux_resource_apply_limits(
	const struct flux_resource_limits *limits)
{
	char buf[128];
	int ret;

#define APPLY_I64(flag, name, field, max_negative)                            \
	do {                                                                   \
		if (limits->flags & (flag)) {                                    \
			ret = flux_resource_write_i64(name, limits->field,          \
						      max_negative);                  \
			if (ret < 0)                                               \
				return ret;                                           \
		}                                                              \
	} while (0)
#define APPLY_U64(flag, name, field)                                          \
	do {                                                                   \
		if (limits->flags & (flag)) {                                    \
			ret = flux_resource_write_u64(name, limits->field);         \
			if (ret < 0)                                               \
				return ret;                                           \
		}                                                              \
	} while (0)
	APPLY_I64(FLUX_RESOURCE_F_MEMORY_MAX, "memory.max", memory_max, true);
	APPLY_I64(FLUX_RESOURCE_F_MEMORY_LOW, "memory.low", memory_low, false);
	APPLY_I64(FLUX_RESOURCE_F_MEMORY_SWAP_MAX, "memory.swap.max",
		  memory_swap_max, true);
	APPLY_U64(FLUX_RESOURCE_F_CPU_WEIGHT, "cpu.weight", cpu_weight);
	if (limits->flags & FLUX_RESOURCE_F_CPU_MAX) {
		if (limits->cpu_quota < 0)
			snprintf(buf, sizeof(buf), "max %llu\n",
				 (unsigned long long)limits->cpu_period);
		else
			snprintf(buf, sizeof(buf), "%lld %llu\n",
				 (long long)limits->cpu_quota,
				 (unsigned long long)limits->cpu_period);
		ret = flux_resource_write("cpu.max", buf);
		if (ret < 0)
			return ret;
	}
	APPLY_U64(FLUX_RESOURCE_F_CPU_BURST, "cpu.max.burst", cpu_burst);
	APPLY_I64(FLUX_RESOURCE_F_CPU_IDLE, "cpu.idle", cpu_idle, false);
	APPLY_I64(FLUX_RESOURCE_F_PIDS_MAX, "pids.max", pids_max, true);
#undef APPLY_I64
#undef APPLY_U64
	return 0;
}

static void flux_resource_io_stats(const char *buf,
				   struct flux_resource_stats *stats)
{
	const char *line = buf;

	while (line && *line) {
		const char *next = strchr(line, '\n');
		struct {
			const char *key;
			u64 *value;
		} fields[] = {
			{ "rbytes=", &stats->io_read_bytes },
			{ "wbytes=", &stats->io_write_bytes },
			{ "rios=", &stats->io_read_ops },
			{ "wios=", &stats->io_write_ops },
		};

		for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
			const char *pos = strnstr(line, fields[i].key,
						  next ? next - line : strlen(line));
			if (pos)
				*fields[i].value += flux_resource_parse_u64(
					pos + strlen(fields[i].key));
		}
		line = next ? next + 1 : NULL;
	}
}

static int flux_resource_collect_stats(struct flux_resource_stats *stats)
{
	char *buf;
	int ret = 0;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memset(stats, 0, sizeof(*stats));
#define READ_SINGLE(name, field)                                              \
	do {                                                                   \
		ret = flux_resource_read(name, buf, PAGE_SIZE);                  \
		if (ret < 0)                                                   \
			goto out;                                            \
		stats->field = flux_resource_parse_u64(buf);                    \
	} while (0)
	READ_SINGLE("memory.current", memory_current);
	READ_SINGLE("memory.max", memory_max);
	READ_SINGLE("pids.current", pids_current);
	READ_SINGLE("pids.max", pids_max);
#undef READ_SINGLE
	ret = flux_resource_read("memory.events", buf, PAGE_SIZE);
	if (ret == 0) {
		stats->memory_events_low = flux_resource_stat_value(buf, "low");
		stats->memory_events_high = flux_resource_stat_value(buf, "high");
		stats->memory_events_max = flux_resource_stat_value(buf, "max");
		stats->memory_events_oom = flux_resource_stat_value(buf, "oom");
		stats->memory_events_oom_kill =
			flux_resource_stat_value(buf, "oom_kill");
	}
	ret = flux_resource_read("cpu.stat", buf, PAGE_SIZE);
	if (ret == 0) {
		stats->cpu_usage_usec = flux_resource_stat_value(buf, "usage_usec");
		stats->cpu_user_usec = flux_resource_stat_value(buf, "user_usec");
		stats->cpu_system_usec =
			flux_resource_stat_value(buf, "system_usec");
		stats->cpu_nr_periods = flux_resource_stat_value(buf, "nr_periods");
		stats->cpu_nr_throttled =
			flux_resource_stat_value(buf, "nr_throttled");
		stats->cpu_throttled_usec =
			flux_resource_stat_value(buf, "throttled_usec");
	}
	ret = flux_resource_read("pids.events", buf, PAGE_SIZE);
	if (ret == 0)
		stats->pids_events_max = flux_resource_stat_value(buf, "max");
	ret = flux_resource_read("io.stat", buf, PAGE_SIZE);
	if (ret == 0)
		flux_resource_io_stats(buf, stats);
	ret = 0;
out:
	kfree(buf);
	return ret;
}

static void flux_resource_dispatch(struct flux_exec_ring *ring)
{
	struct flux_resource_ctrl *resource;
	u64 request_seq;
	int status;

	if (!ring)
		return;
	resource = &ring->resource;
	request_seq = flux_resource_request_seq_load(resource);
	if (!request_seq ||
	    request_seq == flux_resource_response_seq_load(resource))
		return;

	switch (READ_ONCE(resource->op)) {
	case FLUX_RESOURCE_OP_UPDATE:
		status = flux_resource_apply_limits(&resource->limits);
		break;
	case FLUX_RESOURCE_OP_STATS:
		status = flux_resource_collect_stats(&resource->stats);
		break;
	default:
		status = -EINVAL;
		break;
	}
	WRITE_ONCE(resource->status, status);
	flux_resource_response_seq_store(resource, request_seq);
}

void flux_exec_record_init_status(int status)
{
	struct flux_exec_ring *ring = flux_exec_ring();

	/* Direct Flux launches do not map an exec ring.  put_user keeps that
	 * optional case fault-safe; OCI launches publish the raw wait status to
	 * the host runtime through their existing shared ring. */
	(void)put_user(status, &ring->hdr.init_status);
}

static void flux_exec_child_req_free(struct flux_exec_child_req *req)
{
	int i;

	if (!req)
		return;

	kfree(req->filename);
	kfree(req->cwd);
	if (req->argv) {
		for (i = 0; req->argv[i]; i++)
			kfree(req->argv[i]);
		kfree(req->argv);
	}
	if (req->envp) {
		for (i = 0; req->envp[i]; i++)
			kfree(req->envp[i]);
		kfree(req->envp);
	}
	kfree(req);
}

static int flux_exec_dup_string(const struct flux_exec_slot *slot, uint32_t off,
				char **out)
{
	size_t len;

	if (off >= slot->data_len)
		return -EINVAL;

	len = strnlen((const char *)slot->data + off, slot->data_len - off);
	if (off + len >= slot->data_len)
		return -EINVAL;

	*out = kstrdup((const char *)slot->data + off, GFP_KERNEL);
	return *out ? 0 : -ENOMEM;
}

static int flux_exec_dup_vector(const struct flux_exec_slot *slot,
				uint32_t base, uint32_t count, char ***out)
{
	char **vec;
	uint32_t i;
	int ret;

	if (!count) {
		*out = NULL;
		return 0;
	}

	if (base >= slot->data_len ||
	    slot->data_len - base < count * sizeof(uint32_t))
		return -EINVAL;

	vec = kcalloc((size_t)count + 1, sizeof(*vec), GFP_KERNEL);
	if (!vec)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		uint32_t off = ((uint32_t *)(slot->data + base))[i];

		ret = flux_exec_dup_string(slot, off, &vec[i]);
		if (ret < 0) {
			while (i > 0)
				kfree(vec[--i]);
			kfree(vec);
			return ret;
		}
	}

	*out = vec;
	return 0;
}

static int flux_exec_decode_request(const struct flux_exec_slot *slot,
				    struct flux_exec_child_req **out)
{
	struct flux_exec_child_req *req;
	int ret;

	if (!slot || slot->state != FLUX_EXEC_SLOT_READY || !slot->argc)
		return -EINVAL;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ret = flux_exec_dup_string(slot, slot->filename_off, &req->filename);
	if (ret < 0)
		goto err;

	if (slot->flags & FLUX_EXEC_F_HAS_CWD) {
		ret = flux_exec_dup_string(slot, slot->cwd_off, &req->cwd);
		if (ret < 0)
			goto err;
	}

	ret = flux_exec_dup_vector(slot, slot->argv_off, slot->argc,
				   &req->argv);
	if (ret < 0)
		goto err;

	if (slot->flags & FLUX_EXEC_F_HAS_ENV) {
		ret = flux_exec_dup_vector(slot, slot->env_off, slot->envc,
					   &req->envp);
		if (ret < 0)
			goto err;
	}

	if (slot->flags & FLUX_EXEC_F_HAS_USER) {
		req->uid = (uid_t)slot->uid;
		req->gid = (gid_t)slot->gid;
		req->has_user = true;
	}
	req->session_id = slot->session_id;

	*out = req;
	return 0;
err:
	flux_exec_child_req_free(req);
	return ret;
}

static int flux_exec_apply_child_overrides(struct flux_exec_child_req *req)
{
	int ret;

	ret = flux_cons_install_session(req->session_id);
	if (ret < 0) {
		pr_err("failed to install console session %u: %d\n",
		       req->session_id, ret);
		goto out;
	}

	if (req->cwd) {
		struct path path;

		ret = kern_path(req->cwd, LOOKUP_FOLLOW, &path);
		if (ret < 0)
			goto out;
		set_fs_pwd(current->fs, &path);
		path_put(&path);
	}

	if (req->has_user) {
		ret = sys_setgid(req->gid);
		if (ret < 0)
			goto out;
		ret = sys_setuid(req->uid);
		if (ret < 0)
			goto out;
	}

	ret = kernel_execve(req->filename, (const char *const *)req->argv,
			    (const char *const *)req->envp);
	if (ret < 0)
		pr_err("exec failed for %s: %d\n", req->filename, ret);
out:
	return ret;
}

static int flux_exec_child_trampoline(void *arg)
{
	struct flux_exec_child_req *req = arg;
	int ret;

	WARN_ON(!test_thread_flag(TIF_USER));
	WARN_ON(current->flags & PF_KTHREAD);

	ret = flux_exec_apply_child_overrides(req);

	flux_exec_child_req_free(req);

	return ret;
}

static int flux_exec_status_from_wait(int stat)
{
	if (flux_wait_wifexited(stat))
		return flux_wait_wexitstatus(stat);
	if (flux_wait_wifsignaled(stat))
		return 128 + flux_wait_wtermsig(stat);
	return stat;
}

static pid_t flux_exec_fork_child(struct flux_exec_child_req *req)
{
	struct kernel_clone_args args = {
		/* The OCI exec worker must not depend on Flux fork-MM cloning.
		 * vfork keeps the dedicated worker asleep until execve installs the
		 * new image (or the child exits), while avoiding an unnecessary
		 * duplicate of the running container address space. */
		.flags = CLONE_VM | CLONE_VFORK | CLONE_CHILD_CLEARTID |
			 CLONE_CHILD_SETTID,
		.exit_signal = SIGCHLD,
		.fn = flux_exec_child_trampoline,
		.fn_arg = req,
	};
	return kernel_clone(&args);
}

static int flux_exec_worker_main(void *arg)
{
	struct flux_exec_dispatch_req *dispatch = arg;
	struct flux_exec_child_req *req = NULL;
	struct flux_exec_slot *slot;
	int stat = 0, ret;
	pid_t pid;

	if (!dispatch)
		return 0;

	req = dispatch->req;
	slot = dispatch->slot;
	pid = flux_exec_fork_child(req);
	if (pid < 0) {
		pr_err("failed to clone exec child for slot %u: %d\n",
		       dispatch->slot_idx, pid);
		flux_exec_child_req_free(req);
		slot->status = pid;
		flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_ERROR);
		goto out;
	}

	if (slot->flags & FLUX_EXEC_F_DETACH) {
		memset(slot, 0, sizeof(*slot));
		flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_FREE);
		ret = kernel_wait(pid, &stat);
		if (ret < 0)
			pr_warn("detached exec wait failed for slot %u: %d\n",
				dispatch->slot_idx, ret);
		goto out;
	}

	ret = kernel_wait(pid, &stat);
	slot->status = ret < 0 ? ret : flux_exec_status_from_wait(stat);
	flux_exec_slot_state_store(slot, ret < 0 ? FLUX_EXEC_SLOT_ERROR :
						   FLUX_EXEC_SLOT_DONE);
out:
	kfree(dispatch);
	return 0;
}

static int flux_exec_dispatch_ready_slots(struct flux_exec_ring *ring)
{
	uint32_t head, tail, idx;
	int dispatched = 0;

	if (!ring)
		return 0;

	head = ring->hdr.head;
	tail = flux_exec_ring_tail_load(ring);
	idx = head;

	while (idx != tail) {
		struct flux_exec_slot *slot = flux_exec_ring_slot(ring, idx);
		struct flux_exec_dispatch_req *dispatch;
		struct flux_exec_child_req *req;
		pid_t pid;
		int ret;

		if (!slot ||
		    flux_exec_slot_state_load(slot) != FLUX_EXEC_SLOT_READY)
			goto next;

		ret = flux_exec_decode_request(slot, &req);
		if (ret < 0) {
			pr_err("failed to decode exec request at slot=%u: %d\n",
			       idx, ret);
			slot->status = ret;
			flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_ERROR);
			dispatched++;
			goto next;
		}

		flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_RUNNING);

		dispatch = kzalloc(sizeof(*dispatch), GFP_KERNEL);
		if (!dispatch) {
			flux_exec_child_req_free(req);
			slot->status = -ENOMEM;
			flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_ERROR);
			dispatched++;
			goto next;
		}

		dispatch->req = req;
		dispatch->slot = slot;
		dispatch->slot_idx = idx;

		pid = kernel_thread(flux_exec_worker_main, dispatch,
				    "flux_exec",
				    CLONE_FS | CLONE_FILES | SIGCHLD);
		if (pid < 0) {
			pr_err("failed to spawn exec worker for slot %u: %d\n",
			       idx, pid);
			flux_exec_child_req_free(req);
			kfree(dispatch);
			slot->status = pid;
			flux_exec_slot_state_store(slot, FLUX_EXEC_SLOT_ERROR);
			dispatched++;
			goto next;
		}

		dispatched++;
next:
		idx = flux_exec_ring_next_index(ring, idx);
	}

	return dispatched;
}

static int flux_execd_main(void *unused)
{
	struct flux_exec_ring *ring = flux_exec_ring();

	for (;;) {
		wait_event_interruptible(flux_exec_waitq,
					 atomic_read(&flux_exec_pending));
		if (!atomic_xchg(&flux_exec_pending, 0))
			continue;
		flux_resource_dispatch(ring);
		flux_exec_dispatch_ready_slots(ring);
	}

	return 0;
}

int flux_exec_init(void)
{
	pid_t pid;

	pid = kernel_thread(flux_execd_main, NULL, "flux_execd",
			    CLONE_FS | CLONE_FILES | SIGCHLD);
	if (pid < 0) {
		pr_err("failed to create execd main thread: %d\n", pid);
		return -ENOMEM;
	}

	return 0;
}

void flux_exec_wake(void)
{
	atomic_set(&flux_exec_pending, 1);
	wake_up_interruptible(&flux_exec_waitq);
}

#endif /* CONFIG_FLUX_RUNC */
