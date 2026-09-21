#include <errno.h>
#include <fcntl.h>
#include <linux/mman.h>
#include <numaif.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <utils/base.h>
#include <utils/log.h>
#include <utils/memory.h>

void flux_touch_mapping(void *base, size_t len, size_t pgsize)
{
	volatile char *pos;

	/*
     * Unfortunately mmap() provides no error message if MAP_POPULATE fails
     * because of insufficient memory. Therefore, we manually force a write
     * on each page to make sure the mapping was successful.
     */
	for (pos = (volatile char *)base; pos < (volatile char *)base + len;
	     pos += pgsize) {
		WRITE_ONCE(*pos, *pos);
	}
}

static void *__mem_map_common(void *base, size_t len, size_t pgsize, int flags,
			      int fd, unsigned long *mask, int numa_policy)
{
	void *addr;

	if (fd == -1)
		flags |= MAP_ANONYMOUS;
	if (base)
		flags |= MAP_FIXED_NOREPLACE;

	len = align_up(len, pgsize);

	switch (pgsize) {
	case PGSIZE_4KB:
		break;
	case PGSIZE_2MB:
		flags |= MAP_POPULATE;
#ifdef MAP_HUGETLB
		flags |= MAP_HUGETLB;
#endif
#ifdef MAP_HUGE_2MB
		flags |= MAP_HUGE_2MB;
#endif
		break;
	case PGSIZE_1GB:
		flags |= MAP_POPULATE;
#ifdef MAP_HUGETLB
		flags |= MAP_HUGETLB;
#endif
#ifdef MAP_HUGE_1GB
		flags |= MAP_HUGE_1GB;
#endif
		break;
	default: /* fail on other sizes */
		return MAP_FAILED;
	}

	addr = mmap(base, len, PROT_READ | PROT_WRITE, flags, fd, 0);
	if (addr == MAP_FAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "mmap failed %s\n", strerror(errno));
		return MAP_FAILED;
	}

	return addr;
}

/**
 * mem_map_anom - map anonymous memory pages
 * @base: the base address (or NULL for automatic)
 * @len: the length of the mapping
 * @pgsize: the page size
 * @node: the NUMA node
 *
 * Returns the base address, or MAP_FAILED if out of memory
 */
void *flux_mem_map_anom(void *base, size_t len, size_t pgsize, int node)
{
	unsigned long mask = (1 << node);
	/*
	 * This is Flux physical memory, not per-host-process state.  Flux
	 * duplicates host mms to run Flux processes, while Flux page tables
	 * implement the actual fork COW.  Keep the direct map shared across those
	 * host mms so a second host-private COW layer cannot diverge from PFNMAP
	 * user aliases.
	 */
	return __mem_map_common(base, len, pgsize, MAP_SHARED, -1, &mask,
				MPOL_BIND);
}

/**
 * mem_map_shm - maps a System V shared memory segment backed with a file
 * @path: the file path to the shared memory backing file
 * @base: the base address to map the shared segment (or automatic if NULL)
 * @len: the length of the mapping
 * @pgsize: the size of each page
 * @node: the NUMA node
 *
 * Returns a pointer to the mapping, or NULL if the mapping failed.
 */
void *flux_mem_map_shm_file(const char *path, void *base, size_t len,
			   size_t pgsize, int flags, int node, int *retfd)
{
	void *addr;
	int fd;
	unsigned long mask;

	if ((fd = open(path, O_CREAT | O_RDWR, 0666)) < 0)
		return MAP_FAILED;

	if (retfd)
		*retfd = fd;

	len = align_up(len, pgsize);
	if (ftruncate(fd, len) < 0)
		goto err;

	mask = (1 << node);
	if ((addr = __mem_map_common(base, len, pgsize, flags | MAP_SHARED, fd,
				     &mask, MPOL_BIND)) == MAP_FAILED)
		goto err;

	return addr;
err:
	close(fd);
	return MAP_FAILED;
}

/**
 * mem_map_shm - maps a System V shared memory segment
 * @key: the unique key that identifies the shared region (e.g. use ftok())
 * @base: the base address to map the shared segment (or automatic if NULL)
 * @len: the length of the mapping
 * @pgsize: the size of each page
 * @exclusive: ensure this call creates the shared segment
 *
 * Returns a pointer to the mapping, or NULL if the mapping failed.
 */
void *flux_mem_map_shm(mem_key_t key, void *base, size_t len, size_t pgsize,
		      bool exclusive)
{
	void *addr;
	int shmid, flags = IPC_CREAT | 0777;

	switch (pgsize) {
	case PGSIZE_4KB:
		break;
	case PGSIZE_2MB:
		flags |= SHM_HUGETLB;
#ifdef SHM_HUGE_2MB
		flags |= SHM_HUGE_2MB;
#endif
		break;
	case PGSIZE_1GB:
#ifdef SHM_HUGE_1GB
		flags |= SHM_HUGETLB | SHM_HUGE_1GB;
#else
		return MAP_FAILED;
#endif
		break;
	default: /* fail on other sizes */
		return MAP_FAILED;
	}

	if (exclusive)
		flags |= IPC_EXCL;

	shmid = shmget(key, len, flags);
	if (shmid == -1)
		return MAP_FAILED;

	addr = shmat(shmid, base, 0);
	if (addr == MAP_FAILED)
		return MAP_FAILED;

	return addr;
}

/**
 * mem_unmap_shm - detach a shared memory mapping
 * @addr: the base address of the mapping
 *
 * Returns 0 if successful, otherwise fail.
 */
int flux_mem_unmap_shm(void *addr)
{
	if (shmdt(addr) == -1)
		return -errno;
	return 0;
}


/**
 * dump_proc_map - dump the current process's /proc/self/maps
 */
void flux_dump_proc_map(void)
{
	FILE *f;
	ssize_t bytes;
	char *line = NULL;
	size_t len = 0;

	f = fopen("/proc/self/maps", "r");
	if (!f) {
		FLUX_LOG(FLUX_LOG_ERR, "failed to open /proc/self/maps: %s\n",
			strerror(errno));
		return;
	}

	while ((bytes = getline(&line, &len, f)) != -1) {
		printf("%s", line);
	}

	if (line)
		free(line);
	fclose(f);
}
