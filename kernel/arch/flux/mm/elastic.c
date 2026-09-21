// SPDX-License-Identifier: GPL-2.0
/* Experimental independent inventory: no buddy/PCP traffic or memory hotplug. */
#include <linux/bitmap.h>
#include <linux/cache.h>
#include <linux/percpu.h>
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/vmstat.h>
#ifdef CONFIG_FLUX_ELASTIC_MIGRATION
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/swap.h>
/* The pool owns struct pages outside buddy; use the core LRU isolation API. */
#include "../../../mm/internal.h"
#endif
#include <asm/elastic.h>
#include <asm/host_dev.h>

#define EPAGES (FLUX_ELASTIC_EXTENT_SIZE / PAGE_SIZE)
#define NEXTENTS (CONFIG_FLUX_ELASTIC_POOL_MB / 2)
#define LANE_PAGES BITS_PER_LONG
#define LANES (EPAGES / LANE_PAGES)
struct elastic_lane {
	atomic_long_t free;
} ____cacheline_aligned_in_smp;
struct elastic_extent {
	bool draining;
	bool available;
	bool uncertain;
	struct flux_elastic_extent host;
	struct elastic_lane lanes[LANES];
};
/* Independent cache lines keep normal allocation/free local to a lane.
 * No spinlock or interrupt masking is needed in either hot path. */
static struct elastic_extent extents[NEXTENTS];
/* This level changes only with capacity, never on normal page allocation. */
static DECLARE_BITMAP(backed_extents, NEXTENTS);
static DEFINE_PER_CPU(unsigned int, preferred_lane);
struct elastic_cpu_stats { u64 allocations, frees; };
static DEFINE_PER_CPU(struct elastic_cpu_stats, elastic_stats);
static DEFINE_MUTEX(control_lock);
static unsigned long pool_base __read_mostly, pool_end __read_mostly;
static struct zone *pool_zone __read_mostly;
static bool ready __read_mostly, enabled __read_mostly;
static int domain_fd = -1;
static unsigned int target_mb = CONFIG_FLUX_ELASTIC_POOL_MB;
static int last_error;
static u64 drain_attempts, migrated_pages, migration_failures;

static long domain_call(unsigned int cmd, void *arg)
{
	return host_syscall(__NR_ioctl, domain_fd, cmd, arg);
}

void __init flux_elastic_bootmem(unsigned long base, unsigned long size)
{
	struct flux_elastic_create create = { .nr_extents = NEXTENTS };
	long ret;
	unsigned int i;

	BUILD_BUG_ON(PAGE_SHIFT != 12);
	pool_base = base;
	pool_end = base + size;
	ret = flux_host_dev_call_mm(FLUX_DEV_IO_ELASTIC_CREATE, &create);
	if (ret < 0)
		panic("elastic domain create: %ld", ret);
	domain_fd = ret;
	for (i = 0; i < NEXTENTS; i++) {
		extents[i].host.index = i;
		extents[i].host.generation = 1;
		ret = domain_call(FLUX_ELASTIC_COMMIT, &extents[i].host);
		if (ret)
			panic("elastic initial commit: %ld", ret);
		extents[i].available = true;
	}
	ret = host_syscall(__NR_mmap, base, size, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_FIXED_NOREPLACE, domain_fd, 0UL);
	if (ret != base)
		panic("elastic direct map: %lx", ret);
	pr_info("Flux elastic: %u MiB reserved independent inventory\n",
		CONFIG_FLUX_ELASTIC_POOL_MB);
}

bool flux_elastic_contains(struct page *page)
{
	unsigned long addr = page_to_pfn(page) << PAGE_SHIFT;

	return smp_load_acquire(&ready) && addr >= pool_base && addr < pool_end;
}

/* A cleared free bit is ownership: backing cannot be retired after this CAS. */
static __always_inline struct page *take_lane(unsigned int index, unsigned int lane)
{
	struct elastic_lane *l = &extents[index].lanes[lane];
	long free = atomic_long_read(&l->free);
	unsigned long bit;

	do {
		if (!free || !READ_ONCE(enabled) ||
		    (IS_ENABLED(CONFIG_FLUX_ELASTIC_MIGRATION) &&
		     READ_ONCE(extents[index].draining)))
			return NULL;
		bit = __ffs(free);
	} while (!atomic_long_try_cmpxchg(&l->free, &free, free & ~BIT(bit)));
	/* A controller may have closed admission while the CAS was in flight.
	 * Returning our bit is safe: final retirement still atomically captures
	 * every lane and cannot pass while this allocation owns a cleared bit.
	 */
	if (IS_ENABLED(CONFIG_FLUX_ELASTIC_MIGRATION) &&
	    unlikely(READ_ONCE(extents[index].draining))) {
		atomic_long_or(BIT(bit), &l->free);
		return NULL;
	}
	this_cpu_inc(elastic_stats.allocations);
	count_vm_event(PGALLOC_NORMAL);
	return virt_to_page((void *)(pool_base + index * FLUX_ELASTIC_EXTENT_SIZE +
				   (lane * LANE_PAGES + bit) * PAGE_SIZE));
}

struct page *flux_elastic_alloc(struct zone *zone, unsigned int order, gfp_t gfp)
{
	struct page *page;
	unsigned int cpu, hint, first, index, lane, preferred, scanned, scan_lane;

	/* Acquire sees initialized ownership words, page metadata, and zone. */
	if (!smp_load_acquire(&ready) || !READ_ONCE(enabled) || order ||
	    !(gfp & __GFP_MOVABLE) || !gfpflags_allow_blocking(gfp) || zone != pool_zone)
		return NULL;
	/* CPU identity only guides placement. Neither safety nor accounting uses it. */
	cpu = raw_smp_processor_id();
	hint = READ_ONCE(per_cpu(preferred_lane, cpu));
	first = hint / LANES;
	page = take_lane(first, hint % LANES);
	if (page)
		return page;
	preferred = cpu % LANES;
	index = first;
	for (scanned = 0; scanned < NEXTENTS; scanned++, index = (index + 1) % NEXTENTS) {
		if (!test_bit(index, backed_extents))
			continue;
		for (scan_lane = 0; scan_lane < LANES; scan_lane++) {
			lane = (preferred + scan_lane) % LANES;
			page = take_lane(index, lane);
			if (page) {
				WRITE_ONCE(per_cpu(preferred_lane, cpu), index * LANES + lane);
				return page;
			}
		}
	}
	return NULL;
}

/* Publish only after Linux's normal free-page validation and sanitization. */
bool flux_elastic_free(struct page *page, unsigned int order)
{
	struct elastic_lane *l;
	unsigned long offset, index, lane, bit, old;
	unsigned int cpu;

	if (!flux_elastic_contains(page))
		return false;
	BUG_ON(order);
	offset = ((page_to_pfn(page) << PAGE_SHIFT) - pool_base) >> PAGE_SHIFT;
	index = offset / EPAGES;
	lane = (offset % EPAGES) / LANE_PAGES;
	bit = offset % LANE_PAGES;
	l = &extents[index].lanes[lane];
	old = atomic_long_fetch_or(BIT(bit), &l->free);
	BUG_ON(old & BIT(bit));
	this_cpu_inc(elastic_stats.frees);
	count_vm_event(PGFREE);
	/* Prefer the CPU's own lane again once an entire group has returned. */
	cpu = raw_smp_processor_id();
	if (lane == cpu % LANES && (old | BIT(bit)) == ~0UL)
		WRITE_ONCE(per_cpu(preferred_lane, cpu), index * LANES + lane);
	return true;
}

/* All lanes are reserved by the controller before calling the host. */
static void publish_extent(unsigned int i, const struct flux_elastic_extent *h)
{
	struct elastic_extent *e = &extents[i];
	unsigned int lane;

	e->host = *h;
	e->uncertain = false;
	if (h->state == FLUX_ELASTIC_AVAILABLE) {
		e->available = true;
		for (lane = 0; lane < LANES; lane++)
			atomic_long_or(~0UL, &e->lanes[lane].free);
		set_bit(i, backed_extents);
	}
}

/* control_lock serializes transitions. Closing a lane claims all of its free
 * pages with one exchange. If any lane was not entirely free, restore only the
 * bits claimed by this attempt. Concurrent frees concern other bits, so OR
 * preserves them without ever publishing a page owned by an allocator.
 * If every exchange captured all bits, there can be no later real free into
 * those lanes: no allocated pages remain. Their zero words gate allocation
 * until a successful recommit publishes new backing. A stale allocator CAS
 * can only fail while closed, or claim the newly committed logical page.
 */
static int change_extent(unsigned int i, bool commit)
{
	struct elastic_extent *e = &extents[i];
	struct flux_elastic_extent h;
	unsigned long captured[LANES];
	unsigned int lane, done = 0;
	int ret, query = 0;

	if (!commit) {
		for (lane = 0; lane < LANES; lane++)
			if (atomic_long_read(&e->lanes[lane].free) != ~0UL)
				return -EBUSY;
		for (lane = 0; lane < LANES; lane++) {
			captured[lane] = atomic_long_xchg(&e->lanes[lane].free, 0);
			done++;
			if (captured[lane] != ~0UL)
				goto busy;
		}
	}
	e->available = false;
	clear_bit(i, backed_extents);
	h = e->host;
	ret = domain_call(commit ? FLUX_ELASTIC_COMMIT : FLUX_ELASTIC_DECOMMIT, &h);
	/* Successful copyout already contains the authoritative state. */
	if (ret)
		query = domain_call(FLUX_ELASTIC_QUERY, &h);
	if (!query)
		publish_extent(i, &h);
	else
		e->uncertain = true;
	return query ? query : ret;
busy:
	for (lane = 0; lane < done; lane++)
		atomic_long_or(captured[lane], &e->lanes[lane].free);
	return -EBUSY;
}

#ifdef CONFIG_FLUX_ELASTIC_MIGRATION
static struct folio *elastic_migration_target(struct folio *src, unsigned long arg)
{
	/* Without MOVABLE the pool hook is bypassed, even during concurrent
	 * demand. Use bounded allocation effort and avoid invoking OOM for a shrink.
	 */
	return folio_alloc(GFP_HIGHUSER | __GFP_NORETRY | __GFP_NOWARN, 0);
}

static int drain_extent(unsigned int index)
{
	struct elastic_extent *e = &extents[index];
	unsigned long offset;
	unsigned int succeeded = 0;
	LIST_HEAD(pages);
	int ret;

	lockdep_assert_held(&control_lock);
	drain_attempts++;
	WRITE_ONCE(e->draining, true);
	/* Pending LRU additions must become visible to isolation. */
	lru_add_drain_all();
	for (offset = 0; offset < EPAGES; offset++) {
		struct page *page = virt_to_page((void *)(pool_base +
			index * FLUX_ELASTIC_EXTENT_SIZE + offset * PAGE_SIZE));
		struct folio *folio;

		if (!get_page_unless_zero(page))
			continue;
		folio = page_folio(page);
		if (!folio_test_large(folio) && !folio_test_slab(folio) &&
		    folio_isolate_lru(folio)) {
			list_add_tail(&folio->lru, &pages);
			node_stat_mod_folio(folio, NR_ISOLATED_ANON +
					   folio_is_file_lru(folio), 1);
		}
		put_page(page);
	}
	ret = migrate_pages(&pages, elastic_migration_target, NULL, 0,
			    MIGRATE_SYNC, MR_MEMORY_HOTPLUG, &succeeded);
	migrated_pages += succeeded;
	if (ret)
		migration_failures++;
	putback_movable_pages(&pages);
	/* Normal rmap migration replaces PTEs and synchronously revokes their
	 * host aliases through Flux TLB hooks before source pages become free.
	 * The host decommit also checks outstanding pins; no override exists.
	 */
	ret = change_extent(index, false);
	WRITE_ONCE(e->draining, false);
	return ret;
}
#endif

static int set_target(unsigned int mb, unsigned int migration_budget)
{
	unsigned int i, backed = 0, want = mb / 2;
	int ret = 0, rc;

	/* Failed copyout plus failed QUERY stays closed; retry reconciliation here. */
	for (i = 0; i < NEXTENTS; i++) {
		if (extents[i].uncertain) {
			struct flux_elastic_extent h = extents[i].host;

			rc = domain_call(FLUX_ELASTIC_QUERY, &h);
			if (rc)
				return rc;
			publish_extent(i, &h);
		}
	}
	for (i = 0; i < NEXTENTS; i++)
		backed += extents[i].host.state == FLUX_ELASTIC_AVAILABLE;
	/* Drain whole empty extents; lane caches do not hide any free pages. */
	for (i = NEXTENTS; i-- && backed > want;) {
		if (extents[i].host.state != FLUX_ELASTIC_AVAILABLE)
			continue;
		rc = change_extent(i, false);
#ifdef CONFIG_FLUX_ELASTIC_MIGRATION
		if (rc == -EBUSY && migration_budget) {
			migration_budget--;
			rc = drain_extent(i);
		}
#endif
		if (extents[i].host.state == FLUX_ELASTIC_UNBACKED)
			backed--;
		if (rc)
			ret = rc;
	}
	for (i = 0; i < NEXTENTS && backed < want; i++) {
		if (extents[i].host.state != FLUX_ELASTIC_UNBACKED)
			continue;
		rc = change_extent(i, true);
		if (extents[i].host.state == FLUX_ELASTIC_AVAILABLE)
			backed++;
		if (rc) {
			ret = rc;
			break;
		}
	}
	return backed == want ? 0 : (ret ?: -EBUSY);
}

/* All policy and explicit requests use the same serialized state machine. */
int flux_elastic_resize(unsigned int mb, unsigned int migration_budget)
{
	int ret;

	if (mb > CONFIG_FLUX_ELASTIC_POOL_MB || mb % 2)
		return -EINVAL;
	mutex_lock(&control_lock);
	target_mb = mb;
	ret = set_target(mb, migration_budget);
	last_error = ret;
	mutex_unlock(&control_lock);
	return ret;
}

void flux_elastic_snapshot(struct flux_elastic_snapshot *s)
{
	unsigned int i, lane;

	memset(s, 0, sizeof(*s));
	mutex_lock(&control_lock);
	s->capacity_mb = CONFIG_FLUX_ELASTIC_POOL_MB;
	s->target_mb = target_mb;
	s->enabled = READ_ONCE(enabled);
	for (i = 0; i < NEXTENTS; i++) {
		if (!extents[i].available)
			continue;
		s->resident_mb += 2;
		for (lane = 0; lane < LANES; lane++)
			s->free_pages += hweight_long(atomic_long_read(
				&extents[i].lanes[lane].free));
	}
	mutex_unlock(&control_lock);
}

static int elastic_show(struct seq_file *m, void *v)
{
	struct flux_elastic_info info = {};
	unsigned int i, lane, cpu, used = 0, free = 0, blocked = 0;
	u64 a = 0, f = 0;
	bool on;
	int ret;

	mutex_lock(&control_lock);
	ret = domain_call(FLUX_ELASTIC_INFO, &info);
	for (i = 0; i < NEXTENTS; i++) {
		unsigned int extent_free = 0;

		if (!extents[i].available) {
			blocked++;
			continue;
		}
		for (lane = 0; lane < LANES; lane++)
			extent_free += hweight_long(atomic_long_read(&extents[i].lanes[lane].free));
		free += extent_free;
		used += EPAGES - extent_free;
	}
	for_each_possible_cpu(cpu) {
		a += READ_ONCE(per_cpu(elastic_stats, cpu).allocations);
		f += READ_ONCE(per_cpu(elastic_stats, cpu).frees);
	}
	on = READ_ONCE(enabled);
	seq_printf(m, "enabled %u\ncapacity_mb %u\ntarget_mb %u\nused_pages %u\nfree_pages %u\nblocked_extents %u\nallocations %llu\nfrees %llu\nlast_error %d\nhost_error %d\n",
		   on, CONFIG_FLUX_ELASTIC_POOL_MB, target_mb, used, free, blocked,
		   a, f, last_error, ret);
	seq_printf(m, "drain_attempts %llu\nmigrated_pages %llu\nmigration_failures %llu\n",
		   drain_attempts, migrated_pages, migration_failures);
	if (!ret)
		seq_printf(m, "host_resident_pages %llu\nhost_commits %llu\nhost_decommits %llu\nhost_busy %llu\nhost_faults %llu\nhost_denied_faults %llu\n",
			   info.resident_pages, info.commits, info.decommits,
			   info.busy, info.faults, info.denied_faults);
	mutex_unlock(&control_lock);
	return 0;
}

static ssize_t elastic_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *pos)
{
	char input[48], command[16], extra;
	unsigned int value;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = 0;
	if (sscanf(input, "%15s %u %c", command, &value, &extra) != 2)
		return -EINVAL;
	mutex_lock(&control_lock);
	if (!strcmp(command, "enable") && value <= 1) {
		/* Already-started allocations may finish; target still claims free bits
		 * atomically and therefore never relies on disable for safe retirement.
		 */
		WRITE_ONCE(enabled, value);
	} else if (!strcmp(command, "target") && value <= CONFIG_FLUX_ELASTIC_POOL_MB &&
		   !(value % 2)) {
		target_mb = value;
		ret = set_target(value, 0);
#ifdef CONFIG_FLUX_ELASTIC_MIGRATION
	} else if (!strcmp(command, "drain") && value <= CONFIG_FLUX_ELASTIC_POOL_MB &&
		   !(value % 2)) {
		target_mb = value;
		ret = set_target(value, NEXTENTS);
#endif
	} else {
		ret = -EINVAL;
	}
	last_error = ret;
	mutex_unlock(&control_lock);
	return ret ?: count;
}

static int elastic_open(struct inode *inode, struct file *file)
{
	return single_open(file, elastic_show, NULL);
}
static const struct proc_ops elastic_ops = {
	.proc_open = elastic_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = elastic_write,
};

static int __init elastic_init(void)
{
	unsigned long addr;
	unsigned int i, lane, cpu;

	BUILD_BUG_ON(BITS_PER_LONG != 64);
	for (i = 0; i < NEXTENTS; i++) {
		for (lane = 0; lane < LANES; lane++)
			atomic_long_set(&extents[i].lanes[lane].free, ~0UL);
	}
	for_each_possible_cpu(cpu)
		per_cpu(preferred_lane, cpu) = cpu % LANES;
	pool_zone = page_zone(virt_to_page((void *)pool_base));
	for (addr = pool_base; addr < pool_end; addr += PAGE_SIZE) {
		struct page *page = virt_to_page((void *)addr);

		BUG_ON(!PageReserved(page));
		__ClearPageReserved(page);
		set_page_count(page, 0);
	}
	bitmap_fill(backed_extents, NEXTENTS);
	/* Keep this inventory out of zone free/managed counts and watermarks. */
	smp_store_release(&ready, true);
	if (!proc_create("flux_elastic", 0600, NULL, &elastic_ops))
		return -ENOMEM;
	return 0;
}
late_initcall(elastic_init);
