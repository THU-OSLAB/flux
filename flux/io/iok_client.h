#ifndef _FLUX_LIB_IO_IOK_CLIENT_H
#define _FLUX_LIB_IO_IOK_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utils/lrpc.h>

#include "iok_ext.h"

struct flux_fnet_netdev;

struct flux_iok_client_pcpu {
	struct lrpc_chan_out rxq;
	struct lrpc_chan_in txpktq;
	struct lrpc_chan_in txcmdq;
};

int flux_iok_client_init(void);
void flux_iok_client_fini(void);
bool flux_iok_client_is_enabled(void);
bool flux_iok_client_has_network(void);
int flux_iok_client_fill_netdev(struct flux_fnet_netdev *dev);
int flux_iok_client_activate_netdev(void);
const char *flux_iok_sock_path(void);
void *flux_iok_client_dma_alloc(void *hint, size_t len, size_t pgsize,
				int node);
void flux_iok_client_dma_free(void *addr, size_t len, size_t pgsize);
void *flux_iok_client_timer_alloc(int cpu, bool oneshot);
int flux_iok_client_timer_set_oneshot(void *timer, unsigned long ns);

#endif /* _FLUX_LIB_IO_IOK_CLIENT_H */
