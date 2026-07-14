#ifndef _FLUX_IOK_NET_POLL_H
#define _FLUX_IOK_NET_POLL_H

typedef void (*poll_notif_fn_t)(unsigned long pdata, unsigned int event_mask);

struct poll_head {
	poll_notif_fn_t set_fn;
	poll_notif_fn_t clear_fn;
	unsigned long data;
};

static inline void poll_clear(struct poll_head *src, unsigned int event_mask)
{
	if (src->clear_fn)
		src->clear_fn(src->data, event_mask);
}

static inline void poll_set(struct poll_head *src, unsigned int event_mask)
{
	if (src->set_fn)
		src->set_fn(src->data, event_mask);
}


#endif /* _FLUX_IOK_NET_POLL_H */