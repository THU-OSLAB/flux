#ifndef _UTILS_MEMORY_H
#define _UTILS_MEMORY_H

#include <utils/base.h>

/*
 * Shared-memory helpers used by the host runtime and the external dataplane.
 *
 * The mapping policy is implementation-specific, but the contract is that all
 * lengths and page sizes are expressed in bytes.
 */

typedef unsigned int mem_key_t;

/* Prefault a mapping so allocation failures are observed immediately. */
void flux_touch_mapping(void *base, size_t len, size_t pgsize);

/* Map anonymous memory, optionally constrained to a NUMA node. */
void *flux_mem_map_anom(void *base, size_t len, size_t pgsize, int node);

/*
 * Map a shared file-backed region and optionally return the backing fd.
 *
 * Returns MAP_FAILED on failure.
 */
void *flux_mem_map_shm_file(const char *path, void *base, size_t len,
			    size_t pgsize, int flags, int node, int *retfd);

/*
 * Map a System V shared-memory segment identified by @key.
 *
 * Returns MAP_FAILED on failure.
 */
void *flux_mem_map_shm(mem_key_t key, void *base, size_t len, size_t pgsize,
		       bool exclusive);

/* Detach a System V shared-memory mapping created by flux_mem_map_shm(). */
int flux_mem_unmap_shm(void *base);

/* Dump the current host process mappings for diagnostics. */
void flux_dump_proc_map(void);

#endif /* _UTILS_MEMORY_H */
