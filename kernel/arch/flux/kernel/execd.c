#define pr_fmt(fmt) "execd: " fmt

#include <linux/fs_struct.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/idle.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

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

#if defined(CONFIG_FLUX_UINTR) && defined(CONFIG_FLUX_RUNC)

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

static void flux_exec_post_exec(void *entry, unsigned long stack);

static struct flux_exec_ring *flux_exec_ring(void)
{
	return (struct flux_exec_ring *)(unsigned long)FLUX_EXEC_RING_MAP_ADDR;
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
	const struct flux_cons *cons;
	void *old_journal_info;
	int ret;

	cons = flux_cons_lookup(req->session_id);
	if (!cons)
		return -EINVAL;

	switch (cons->kind) {
	case FLUX_CONS_KIND_NONE:
		break;
	case FLUX_CONS_KIND_STDIO:
		ret = flux_cons_install_stdio(cons);
		if (ret < 0) {
			pr_err("failed to install stdio session %u: %d\n",
			       req->session_id, ret);
			goto out;
		}
		break;
	case FLUX_CONS_KIND_TTY:
		ret = flux_cons_install_tty(cons);
		if (ret < 0) {
			pr_err("failed to install tty session %u: %d\n",
			       req->session_id, ret);
			goto out;
		}
		break;
	default:
		ret = -EINVAL;
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

	old_journal_info = current->journal_info;
	current->journal_info = req;
	ret = flux_do_host_exec_with_post(req->filename, req->argv, req->envp,
					  flux_exec_post_exec);
	current->journal_info = old_journal_info;
	if (ret < 0)
		pr_err("host exec failed for %s: %d\n", req->filename, ret);
out:
	return ret;
}

static void flux_exec_post_exec(void *entry, unsigned long stack)
{
	struct flux_exec_child_req *req = current->journal_info;

	current->journal_info = NULL;
	flux_exec_child_req_free(req);
	flux_post_exec_to_user(entry, stack);
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
		.flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID,
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

#endif /* CONFIG_FLUX_UINTR && CONFIG_FLUX_RUNC */
