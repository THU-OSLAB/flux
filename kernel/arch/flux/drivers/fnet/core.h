#ifndef _FLUX_IOK_CORE_H
#define _FLUX_IOK_CORE_H

#include <linux/sched/clock.h>
#include <linux/types.h>
#include <linux/time.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/if_ether.h>
#include <asm/x86/tsc.h>

#include "mbuf.h"

struct ethaddr {
	uint8_t addr[6];
} __packed;

struct mbuf;
union flux_rxq_cmd;

extern struct mbuf *net_rx_alloc_mbuf(unsigned long data,
				      union flux_rxq_cmd cmd);
extern void net_rx_trans(struct mbuf *m);
extern void net_rx_batch(struct mbuf **ms, unsigned int nr);
extern bool net_rx_fast_tcp(struct mbuf *m);
extern void tcp_rx_closed(struct mbuf *m);
extern void tcp_free_rx_bufs(void);

extern struct mbuf *net_tx_alloc_mbuf(size_t header_len);
extern void net_tx_release_mbuf(struct mbuf *m);
extern void net_tx_eth(struct mbuf *m, uint16_t proto,
		       const struct ethaddr *dhost);
extern int net_tx_ip(struct mbuf *m, uint8_t proto, uint32_t daddr);

static inline size_t eth_headroom(void)
{
	return sizeof(struct ethhdr);
}

static inline size_t ip_headroom(void)
{
	return eth_headroom() + sizeof(struct iphdr);
}

/**
 * wraps_lt - a < b ?
 *
 * This comparison is safe against unsigned wrap around.
 */
static inline bool wraps_lt(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

/**
 * wraps_lte - a <= b ?
 *
 * This comparison is safe against unsigned wrap around.
 */
static inline bool wraps_lte(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) <= 0;
}

/**
 * wraps_gt - a > b ?
 *
 * This comparison is safe against unsigned wrap around.
 */
static inline bool wraps_gt(uint32_t a, uint32_t b)
{
	return (int32_t)(b - a) < 0;
}

/**
 * wraps_gte - a >= b ?
 *
 * This comparison is safe against unsigned wrap around.
 */
static inline bool wraps_gte(uint32_t a, uint32_t b)
{
	return (int32_t)(b - a) <= 0;
}

static inline uint64_t now_us(void)
{
	return sched_clock() / NSEC_PER_USEC;
}

static inline uint32_t __crc32c_u64(uint64_t crc, uint64_t val)
{
	asm volatile("crc32q %1, %0"
		     : "+r"(crc) /* %0: crc updated in-place */
		     : "r"(val) /* %1: 64-bit value */
	);
	return crc;
}

/**
 * hash_crc32c_one - hashes one 64-bit word
 * @seed: useful for creating multiple hash functions
 * @val: the word to hash
 *
 * Returns a 32-bit hash value.
 */
static inline uint32_t hash_crc32c_one(uint32_t seed, uint64_t val)
{
	return __crc32c_u64(seed, val);
}

/**
 * hash_crc32c_two - hashes two 64-bit words
 * @seed: useful for creating multiple hash functions
 * @a: the first word to hash
 * @b: the second word to hash
 *
 * Returns a 32-bit hash value.
 */
static inline uint32_t hash_crc32c_two(uint32_t seed, uint64_t a, uint64_t b)
{
	seed = __crc32c_u64(seed, a);
	return __crc32c_u64(seed, b);
}

/**
 * rand_crc32c - generates a very fast pseudorandom value using crc32c
 * @seed: a seed-value for the hash
 *
 * WARNING: not a cryptographic hash.
 */
static inline uint64_t rand_crc32c(uint32_t seed)
{
	return hash_crc32c_one(seed, rdtsc());
}

#endif /* _FLUX_IOK_CORE_H */
