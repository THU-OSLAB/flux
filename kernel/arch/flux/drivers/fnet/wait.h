/* wait queue operations */
#ifndef _FLUX_IOK_NET_WAIT_H
#define _FLUX_IOK_NET_WAIT_H

#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>

struct tcp_wait_queue_entry;

typedef int (*tcp_wait_queue_func_t)(struct tcp_wait_queue_entry *wq_entry,
				     unsigned mode, int flags, void *key);

struct tcp_wait_queue_head {
	struct list_head head;
};

struct tcp_wait_queue_entry {
	unsigned int flags; /* used for padding */
	struct task_struct *p;
	struct list_head entry;
	tcp_wait_queue_func_t func;
};
typedef struct tcp_wait_queue_head tcp_wait_queue_head_t;
typedef struct tcp_wait_queue_entry tcp_wait_queue_entry_t;

#define tcp_wait_queue_init(wq_head) INIT_LIST_HEAD(&(wq_head)->head)
#define tcp_wait_queue_entry_init_func(wait, __func)                    \
	do {                                                            \
		(wait)->p = current;                                    \
		(wait)->func = (tcp_wait_queue_func_t)(void *)(__func); \
		INIT_LIST_HEAD(&(wait)->entry);                         \
	} while (0)
#define tcp_wait_queue_entry_init(wait) \
	tcp_wait_queue_entry_init_func(wait, default_wake_function)
#define tcp_wait_queue_empty(wq_head) list_empty(&(wq_head)->head)

/**
 * tcp_wait_queue_wake_up_all - wake up all waiters on a TCP wait queue
 *
 * @wq_head: the wait queue head
 */
static inline void tcp_wait_queue_wake_up_all(tcp_wait_queue_head_t *wq_head)
{
	tcp_wait_queue_entry_t *wq_entry, *tmp;

	list_for_each_entry_safe(wq_entry, tmp, &wq_head->head, entry) {
		list_del(&wq_entry->entry);
		wq_entry->func(wq_entry, TASK_NORMAL, 0, NULL);
	}
}

/**
 * tcp_wait_queue_wake_up_all_start - move all waiters to a separate list
 *
 * @wq_head: the wait queue head
 * @waiters: the list to move waiters to
 */
static inline void
tcp_wait_queue_wake_up_all_start(struct tcp_wait_queue_head *wq_head,
				 struct list_head *waiters)
{
	tcp_wait_queue_entry_t *wq_entry, *tmp;

	list_for_each_entry_safe(wq_entry, tmp, &wq_head->head, entry) {
		list_del(&wq_entry->entry);
		list_add(&wq_entry->entry, waiters);
	}
}

static inline void tcp_wait_queue_wake_up_all_finish(struct list_head *waiters)
{
	tcp_wait_queue_entry_t *wq_entry, *tmp;

	list_for_each_entry_safe(wq_entry, tmp, waiters, entry) {
		list_del(&wq_entry->entry);
		wq_entry->func(wq_entry, TASK_NORMAL, 0, NULL);
	}
}

static inline tcp_wait_queue_entry_t *
tcp_wait_queue_signal_start(tcp_wait_queue_head_t *wq_head)
{
	tcp_wait_queue_entry_t *wq_entry;

	wq_entry = list_first_entry_or_null(&wq_head->head,
					    struct tcp_wait_queue_entry, entry);
	if (wq_entry)
		list_del(&wq_entry->entry);
	return wq_entry;
}

static inline void
tcp_wait_queue_signal_finish(tcp_wait_queue_entry_t *wq_entry)
{
	if (wq_entry)
		wq_entry->func(wq_entry, TASK_NORMAL, 0, NULL);
}

#define tcp_wait_queue_signal(wq_head)                              \
	({                                                          \
		tcp_wait_queue_entry_t *___wq_entry;                \
                                                                    \
		___wq_entry = tcp_wait_queue_signal_start(wq_head); \
		tcp_wait_queue_signal_finish(___wq_entry);          \
	})

/**
 * tcp_wait_queue_wait - wait on a TCP wait queue
 *
 * @wq_head: the wait queue head
 * @lock: a held spinlock protecting the wake queue and the condition
 * @state: the task state to set while waiting
 */
static inline void tcp_wait_queue_wait(tcp_wait_queue_head_t *wq_head,
				       spinlock_t *lock, int state)
{
	tcp_wait_queue_entry_t wq_entry;

	tcp_wait_queue_entry_init(&wq_entry);

	list_add(&wq_entry.entry, &wq_head->head);

	spin_unlock(lock);

	set_current_state(state);

	schedule();

	__set_current_state(TASK_RUNNING);

	spin_lock(lock);
}

#endif /* _FLUX_IOK_NET_WAIT_H */