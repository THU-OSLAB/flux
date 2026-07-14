#define FLUX_FMT "iokd-timer: "

#include <utils/time.h>

#include "iokd.h"

#define FLUX_IOKD_TIMER_POLL_NS 10000

bool flux_iokd_timers_run(void)
{
	static uint64_t next_poll_ns;
	uint64_t now = now_ns();
	bool work_done = false;
	int i;
	int j;

	if ((int64_t)(now - next_poll_ns) < 0)
		return false;

	next_poll_ns = now + FLUX_IOKD_TIMER_POLL_NS;

	for (i = 0; i < FLUX_IOKD_MAX_CLIENTS; i++) {
		struct flux_iokd_client *client = flux_iokd_client_load(i);
		struct flux_iok_timer_entry *timers;

		if (!client || !flux_iokd_client_ready(client) || !client->timer_base)
			continue;

		timers = client->timer_base;
		for (j = 0; j < client->nr_cpus; j++) {
			uint64_t deadline;

			deadline = atomic_load_acquire(&timers[j].deadline_ns);
			if (!deadline || deadline > now)
				continue;
			if (!atomic_cmpxchg_acq_rel_acquire(&timers[j].deadline_ns,
							   &deadline, 0))
				continue;

			flux_iokd_notify_timer(client, j);
			work_done = true;
		}
	}

	return work_done;
}
