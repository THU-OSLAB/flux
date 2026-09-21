#ifndef _FLUX_NET_H
#define _FLUX_NET_H

#include <flux/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Network configuration helpers for application-facing interfaces.
 *
 * The plain gateway helpers update the default routing table. Interfaces that
 * take an ifindex operate on per-interface policy-routing tables derived from
 * that interface.
 */
int flux_if_add_ip(int ifindex, int af, void *addr, unsigned int netprefix_len);
int flux_if_del_ip(int ifindex, int af, void *addr, unsigned int netprefix_len);
int flux_add_gateway(int af, void *gwaddr);

/*
 * Policy-routing tables are derived from the interface index:
 *   ipv4 -> ifindex * 2 + 0
 *   ipv6 -> ifindex * 2 + 1
 */
int flux_if_add_rule_from_saddr(int ifindex, int af, void *saddr);
int flux_if_add_gateway(int ifindex, int af, void *gwaddr);
int flux_if_add_linklocal(int ifindex, int af, void *addr, int netprefix_len);

/* IPv6 DAD must be waited on only after the interface is already up. */
int flux_if_wait_ipv6_dad(int ifindex, void *addr);

int flux_if_up(int ifindex);
int flux_if_down(int ifindex);
int flux_if_set_mtu(int ifindex, int mtu);

int flux_if_set_ipv4(int ifindex, unsigned int addr, unsigned int netmask_len);
int flux_set_ipv4_gateway(unsigned int addr);
int flux_if_set_ipv4_gateway(int ifindex, unsigned int addr,
			     unsigned int netmask_len, unsigned int gw_addr);

int flux_if_set_ipv6(int ifindex, void *addr, unsigned int netprefix_len);
int flux_set_ipv6_gateway(void *addr);
int flux_if_set_ipv6_gateway(int ifindex, void *addr, unsigned int netmask_len,
			     void *gw_addr);

int flux_ifname_to_ifindex(const char *ifname);
int flux_if_dump_ipv4_addr(const char *ifname);
int flux_if_dump_ipv4_rules(const char *ifname);

#ifdef __cplusplus
}
#endif

#endif
