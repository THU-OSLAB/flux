// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include <flux.h>

#include "endian.h"

static inline int ifindex_to_name(int sock, struct flux_ifreq *ifr, int ifindex)
{
	ifr->flux_ifr_ifindex = ifindex;
	return flux_sys_ioctl(sock, FLUX_SIOCGIFNAME, (long)ifr);
}

int flux_ifname_to_ifindex(const char *ifname)
{
	struct flux_ifreq ifr;
	int fd, ret;

	fd = flux_sys_socket(FLUX_AF_INET, FLUX_SOCK_DGRAM, 0);
	if (fd < 0)
		return fd;

	strcpy(ifr.flux_ifr_name, ifname);

	ret = flux_sys_ioctl(fd, FLUX_SIOCGIFINDEX, (long)&ifr);
	if (ret < 0)
		return ret;

	return ifr.flux_ifr_ifindex;
}

int flux_if_up(int ifindex)
{
	struct flux_ifreq ifr;
	int err, sock = flux_sys_socket(FLUX_AF_INET, FLUX_SOCK_DGRAM, 0);

	if (sock < 0)
		return sock;
	err = ifindex_to_name(sock, &ifr, ifindex);
	if (err < 0)
		return err;

	err = flux_sys_ioctl(sock, FLUX_SIOCGIFFLAGS, (long)&ifr);
	if (!err) {
		ifr.flux_ifr_flags |= FLUX_IFF_UP;
		err = flux_sys_ioctl(sock, FLUX_SIOCSIFFLAGS, (long)&ifr);
	}

	flux_sys_close(sock);

	return err;
}

int flux_if_down(int ifindex)
{
	struct flux_ifreq ifr;
	int err, sock;

	sock = flux_sys_socket(FLUX_AF_INET, FLUX_SOCK_DGRAM, 0);
	if (sock < 0)
		return sock;

	err = ifindex_to_name(sock, &ifr, ifindex);
	if (err < 0)
		return err;

	err = flux_sys_ioctl(sock, FLUX_SIOCGIFFLAGS, (long)&ifr);
	if (!err) {
		ifr.flux_ifr_flags &= ~FLUX_IFF_UP;
		err = flux_sys_ioctl(sock, FLUX_SIOCSIFFLAGS, (long)&ifr);
	}

	flux_sys_close(sock);

	return err;
}

int flux_if_set_mtu(int ifindex, int mtu)
{
	struct flux_ifreq ifr;
	int err, sock;

	sock = flux_sys_socket(FLUX_AF_INET, FLUX_SOCK_DGRAM, 0);
	if (sock < 0)
		return sock;

	err = ifindex_to_name(sock, &ifr, ifindex);
	if (err < 0)
		return err;

	ifr.flux_ifr_mtu = mtu;

	err = flux_sys_ioctl(sock, FLUX_SIOCSIFMTU, (long)&ifr);

	flux_sys_close(sock);

	return err;
}

int flux_if_set_ipv4(int ifindex, unsigned int addr, unsigned int netmask_len)
{
	return flux_if_add_ip(ifindex, FLUX_AF_INET, &addr, netmask_len);
}

int flux_if_set_ipv4_gateway(int ifindex, unsigned int src_addr,
			    unsigned int src_masklen, unsigned int via_addr)
{
	int err;

	err = flux_if_add_rule_from_saddr(ifindex, FLUX_AF_INET, &src_addr);
	if (err)
		return err;
	err = flux_if_add_linklocal(ifindex, FLUX_AF_INET, &src_addr,
				   src_masklen);
	if (err)
		return err;
	return flux_if_add_gateway(ifindex, FLUX_AF_INET, &via_addr);
}

int flux_set_ipv4_gateway(unsigned int addr)
{
	return flux_add_gateway(FLUX_AF_INET, &addr);
}

int flux_netdev_get_ifindex(int id)
{
	struct flux_ifreq ifr;
	int sock, ret;

	sock = flux_sys_socket(FLUX_AF_INET, FLUX_SOCK_DGRAM, 0);
	if (sock < 0)
		return sock;

	snprintf(ifr.flux_ifr_name, sizeof(ifr.flux_ifr_name), "eth%d", id);
	ret = flux_sys_ioctl(sock, FLUX_SIOCGIFINDEX, (long)&ifr);
	flux_sys_close(sock);

	return ret < 0 ? ret : ifr.flux_ifr_ifindex;
}

static int netlink_sock(unsigned int groups)
{
	struct flux_sockaddr_nl la;
	int fd, err;

	fd = flux_sys_socket(FLUX_AF_NETLINK, FLUX_SOCK_DGRAM, FLUX_NETLINK_ROUTE);
	if (fd < 0)
		return fd;

	memset(&la, 0, sizeof(la));
	la.nl_family = FLUX_AF_NETLINK;
	la.nl_groups = groups;
	err = flux_sys_bind(fd, (struct flux_sockaddr *)&la, sizeof(la));
	if (err < 0)
		return err;

	return fd;
}

static int parse_rtattr(struct flux_rtattr *tb[], int max,
			struct flux_rtattr *rta, int len)
{
	unsigned short type;

	memset(tb, 0, sizeof(struct flux_rtattr *) * (max + 1));
	while (FLUX_RTA_OK(rta, len)) {
		type = rta->rta_type;
		if ((type <= max) && (!tb[type]))
			tb[type] = rta;
		rta = FLUX_RTA_NEXT(rta, len);
	}
	if (len)
		FLUX_LOG(FLUX_LOG_INFO, "!!!Deficit %d, rta_len=%d\n", len,
			rta->rta_len);
	return 0;
}

struct addr_filter {
	unsigned int ifindex;
	void *addr;
};

static unsigned int get_ifa_flags(struct flux_ifaddrmsg *ifa,
				  struct flux_rtattr *ifa_flags_attr)
{
	return ifa_flags_attr ? *(unsigned int *)FLUX_RTA_DATA(ifa_flags_attr) :
				ifa->ifa_flags;
}

/* returns:
 * 0 - dad succeed.
 * -1 - dad failed or other error.
 * 1 - should wait for new msg.
 */
static int check_ipv6_dad(struct flux_sockaddr_nl *nladdr,
			  struct flux_nlmsghdr *n, void *arg)
{
	struct addr_filter *filter = arg;
	struct flux_ifaddrmsg *ifa = FLUX_NLMSG_DATA(n);
	struct flux_rtattr *rta_tb[FLUX_IFA_MAX + 1];
	unsigned int ifa_flags;
	int len = n->nlmsg_len;

	if (n->nlmsg_type != FLUX_RTM_NEWADDR)
		return 1;

	len -= FLUX_NLMSG_LENGTH(sizeof(*ifa));
	if (len < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "BUG: wrong nlmsg len %d\n", len);
		return -1;
	}

	parse_rtattr(rta_tb, FLUX_IFA_MAX, FLUX_IFA_RTA(ifa),
		     n->nlmsg_len - FLUX_NLMSG_LENGTH(sizeof(*ifa)));

	ifa_flags = get_ifa_flags(ifa, rta_tb[FLUX_IFA_FLAGS]);

	if (ifa->ifa_index != filter->ifindex)
		return 1;
	if (ifa->ifa_family != FLUX_AF_INET6)
		return 1;

	if (!rta_tb[FLUX_IFA_LOCAL])
		rta_tb[FLUX_IFA_LOCAL] = rta_tb[FLUX_IFA_ADDRESS];

	if (!rta_tb[FLUX_IFA_LOCAL] ||
	    (filter->addr &&
	     memcmp(FLUX_RTA_DATA(rta_tb[FLUX_IFA_LOCAL]), filter->addr, 16))) {
		return 1;
	}
	if (ifa_flags & FLUX_IFA_F_DADFAILED) {
		FLUX_LOG(FLUX_LOG_ERR, "IPV6 DAD failed.\n");
		return -1;
	}
	if (!(ifa_flags & FLUX_IFA_F_TENTATIVE))
		return 0;
	return 1;
}

/* Copied from iproute2/lib/ */
static int rtnl_listen(int fd,
		       int (*handler)(struct flux_sockaddr_nl *nladdr,
				      struct flux_nlmsghdr *, void *),
		       void *arg)
{
	int status;
	struct flux_nlmsghdr *h;
	struct flux_sockaddr_nl nladdr = { .nl_family = FLUX_AF_NETLINK };
	struct flux_iovec iov;
	struct flux_user_msghdr msg = {
		.msg_name = &nladdr,
		.msg_namelen = sizeof(nladdr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	char buf[16384];

	iov.iov_base = buf;
	while (1) {
		iov.iov_len = sizeof(buf);
		status = flux_sys_recvmsg(fd, &msg, 0);
		if (status < 0) {
			if (status == -FLUX_EINTR || status == -FLUX_EAGAIN)
				continue;
			FLUX_LOG(FLUX_LOG_ERR, "netlink receive error %s (%d)\n",
				flux_strerror(status), status);
			if (status == -FLUX_ENOBUFS)
				continue;
			return -1;
		}
		if (status == 0) {
			FLUX_LOG(FLUX_LOG_ERR, "EOF on netlink\n");
			return -1;
		}
		if (msg.msg_namelen != sizeof(nladdr)) {
			FLUX_LOG(FLUX_LOG_ERR, "sender address length == %d\n",
				msg.msg_namelen);
			return -1;
		}

		for (h = (struct flux_nlmsghdr *)buf;
		     (unsigned int)status >= sizeof(*h);) {
			int err;
			int len = h->nlmsg_len;
			int l = len - sizeof(*h);

			if (l < 0 || len > status) {
				if (msg.msg_flags & FLUX_MSG_TRUNC) {
					FLUX_LOG(FLUX_LOG_ERR,
						"Truncated message\n");
					return -1;
				}
				FLUX_LOG(FLUX_LOG_ERR,
					"!!!malformed message: len=%d\n", len);
				return -1;
			}

			err = handler(&nladdr, h, arg);
			if (err <= 0)
				return err;

			status -= FLUX_NLMSG_ALIGN(len);
			h = (struct flux_nlmsghdr *)((char *)h +
						    FLUX_NLMSG_ALIGN(len));
		}
		if (msg.msg_flags & FLUX_MSG_TRUNC) {
			FLUX_LOG(FLUX_LOG_DEBUG, "Message truncated\n");
			continue;
		}
		if (status) {
			FLUX_LOG(FLUX_LOG_ERR, "!!!Remnant of size %d\n", status);
			return -1;
		}
	}

	return 0;
}

int flux_if_wait_ipv6_dad(int ifindex, void *addr)
{
	struct addr_filter filter = { .ifindex = ifindex, .addr = addr };
	int fd, ret;
	struct {
		struct flux_nlmsghdr nlmsg_info;
		struct flux_ifaddrmsg ifaddrmsg_info;
	} req;

	fd = netlink_sock(1 << (FLUX_RTNLGRP_IPV6_IFADDR - 1));
	if (fd < 0)
		return fd;

	memset(&req, 0, sizeof(req));
	req.nlmsg_info.nlmsg_len =
		FLUX_NLMSG_LENGTH(sizeof(struct flux_ifaddrmsg));
	req.nlmsg_info.nlmsg_flags = FLUX_NLM_F_REQUEST | FLUX_NLM_F_DUMP;
	req.nlmsg_info.nlmsg_type = FLUX_RTM_GETADDR;
	req.ifaddrmsg_info.ifa_family = FLUX_AF_INET6;
	req.ifaddrmsg_info.ifa_index = ifindex;
	ret = flux_sys_send(fd, &req, req.nlmsg_info.nlmsg_len, 0);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "flux_sys_send failed: %d\n", ret);
		return ret;
	}
	ret = rtnl_listen(fd, check_ipv6_dad, (void *)&filter);
	flux_sys_close(fd);
	return ret;
}

int flux_if_set_ipv6(int ifindex, void *addr, unsigned int netprefix_len)
{
	int err = flux_if_add_ip(ifindex, FLUX_AF_INET6, addr, netprefix_len);
	if (err)
		return err;
	return flux_if_wait_ipv6_dad(ifindex, addr);
}

int flux_if_set_ipv6_gateway(int ifindex, void *src_addr,
			    unsigned int src_masklen, void *via_addr)
{
	int err;

	err = flux_if_add_rule_from_saddr(ifindex, FLUX_AF_INET6, src_addr);
	if (err)
		return err;
	err = flux_if_add_linklocal(ifindex, FLUX_AF_INET6, src_addr,
				   src_masklen);
	if (err)
		return err;
	return flux_if_add_gateway(ifindex, FLUX_AF_INET6, via_addr);
}

int flux_set_ipv6_gateway(void *addr)
{
	return flux_add_gateway(FLUX_AF_INET6, addr);
}

/* returns:
 * 0 - succeed.
 * < 0 - error number.
 * 1 - should wait for new msg.
 */
static int check_error(struct flux_sockaddr_nl *nladdr, struct flux_nlmsghdr *n,
		       void *arg)
{
	unsigned int s = *(unsigned int *)arg;

	if (nladdr->nl_pid != 0 || n->nlmsg_seq != s) {
		/* Don't forget to skip that message. */
		return 1;
	}

	if (n->nlmsg_type == FLUX_NLMSG_ERROR) {
		struct flux_nlmsgerr *err =
			(struct flux_nlmsgerr *)FLUX_NLMSG_DATA(n);
		int l = n->nlmsg_len - sizeof(*n);

		if (l < (int)sizeof(struct flux_nlmsgerr))
			FLUX_LOG(FLUX_LOG_ERR, "ERROR truncated\n");
		else if (!err->error)
			return 0;

		FLUX_LOG(FLUX_LOG_ERR, "RTNETLINK answers: %s\n",
			flux_strerror(-err->error));
		return err->error;
	}
	FLUX_LOG(FLUX_LOG_ERR, "Unexpected reply!!!\n");
	return -1;
}

static unsigned int seq;
static int rtnl_talk(int fd, struct flux_nlmsghdr *n)
{
	int status;
	struct flux_sockaddr_nl nladdr = { .nl_family = FLUX_AF_NETLINK };
	struct flux_iovec iov = { .iov_base = (void *)n,
				 .iov_len = n->nlmsg_len };
	struct flux_user_msghdr msg = {
		.msg_name = &nladdr,
		.msg_namelen = sizeof(nladdr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	n->nlmsg_seq = seq;
	n->nlmsg_flags |= FLUX_NLM_F_ACK;

	status = flux_sys_sendmsg(fd, &msg, 0);
	if (status < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "Cannot talk to rtnetlink: %d\n", status);
		return status;
	}

	status = rtnl_listen(fd, check_error, (void *)&seq);
	seq++;
	return status;
}

static int addattr_l(struct flux_nlmsghdr *n, unsigned int maxlen, int type,
		     const void *data, int alen)
{
	int len = FLUX_RTA_LENGTH(alen);
	struct flux_rtattr *rta;

	if (FLUX_NLMSG_ALIGN(n->nlmsg_len) + FLUX_RTA_ALIGN(len) > maxlen) {
		FLUX_LOG(FLUX_LOG_ERR,
			"addattr_l ERROR: message exceeded bound of %d\n",
			maxlen);
		return -1;
	}
	rta = ((struct flux_rtattr *)(((void *)(n)) +
				     FLUX_NLMSG_ALIGN(n->nlmsg_len)));
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(FLUX_RTA_DATA(rta), data, alen);
	n->nlmsg_len = FLUX_NLMSG_ALIGN(n->nlmsg_len) + FLUX_RTA_ALIGN(len);
	return 0;
}

int flux_add_neighbor(int ifindex, int af, void *ip, void *mac)
{
	struct {
		struct flux_nlmsghdr n;
		struct flux_ndmsg r;
		char buf[1024];
	} req = {
		.n.nlmsg_len = FLUX_NLMSG_LENGTH(sizeof(struct flux_ndmsg)),
		.n.nlmsg_type = FLUX_RTM_NEWNEIGH,
		.n.nlmsg_flags = FLUX_NLM_F_REQUEST | FLUX_NLM_F_CREATE |
				 FLUX_NLM_F_REPLACE,
		.r.ndm_family = af,
		.r.ndm_ifindex = ifindex,
		.r.ndm_state = FLUX_NUD_PERMANENT,

	};
	int err, addr_sz;
	int fd;

	if (af == FLUX_AF_INET)
		addr_sz = 4;
	else if (af == FLUX_AF_INET6)
		addr_sz = 16;
	else {
		FLUX_LOG(FLUX_LOG_ERR, "Bad address family: %d\n", af);
		return -1;
	}

	fd = netlink_sock(0);
	if (fd < 0)
		return fd;

	// create the IP attribute
	addattr_l(&req.n, sizeof(req), FLUX_NDA_DST, ip, addr_sz);

	// create the MAC attribute
	addattr_l(&req.n, sizeof(req), FLUX_NDA_LLADDR, mac, 6);

	err = rtnl_talk(fd, &req.n);
	flux_sys_close(fd);
	return err;
}

static int ipaddr_modify(int cmd, int flags, int ifindex, int af, void *addr,
			 unsigned int netprefix_len)
{
	struct {
		struct flux_nlmsghdr n;
		struct flux_ifaddrmsg ifa;
		char buf[256];
	} req = {
		.n.nlmsg_len = FLUX_NLMSG_LENGTH(sizeof(struct flux_ifaddrmsg)),
		.n.nlmsg_flags = FLUX_NLM_F_REQUEST | flags,
		.n.nlmsg_type = cmd,
		.ifa.ifa_family = af,
		.ifa.ifa_prefixlen = netprefix_len,
		.ifa.ifa_index = ifindex,
	};
	int err, addr_sz;
	int fd;

	if (af == FLUX_AF_INET)
		addr_sz = 4;
	else if (af == FLUX_AF_INET6)
		addr_sz = 16;
	else {
		FLUX_LOG(FLUX_LOG_ERR, "Bad address family: %d\n", af);
		return -1;
	}

	fd = netlink_sock(0);
	if (fd < 0)
		return fd;

	// create the IP attribute
	addattr_l(&req.n, sizeof(req), FLUX_IFA_LOCAL, addr, addr_sz);

	err = rtnl_talk(fd, &req.n);

	flux_sys_close(fd);
	return err;
}

int flux_if_add_ip(int ifindex, int af, void *addr, unsigned int netprefix_len)
{
	return ipaddr_modify(FLUX_RTM_NEWADDR, FLUX_NLM_F_CREATE | FLUX_NLM_F_EXCL,
			     ifindex, af, addr, netprefix_len);
}

int flux_if_del_ip(int ifindex, int af, void *addr, unsigned int netprefix_len)
{
	return ipaddr_modify(FLUX_RTM_DELADDR, 0, ifindex, af, addr,
			     netprefix_len);
}

static int iproute_modify(int cmd, unsigned int flags, int ifindex, int af,
			  void *route_addr, int route_masklen, void *gwaddr)
{
	struct {
		struct flux_nlmsghdr n;
		struct flux_rtmsg r;
		char buf[1024];
	} req = {
		.n.nlmsg_len = FLUX_NLMSG_LENGTH(sizeof(struct flux_rtmsg)),
		.n.nlmsg_flags = FLUX_NLM_F_REQUEST | flags,
		.n.nlmsg_type = cmd,
		.r.rtm_family = af,
		.r.rtm_table = FLUX_RT_TABLE_MAIN,
		.r.rtm_scope = FLUX_RT_SCOPE_UNIVERSE,
	};
	int err, addr_sz;
	int i, fd;

	fd = netlink_sock(0);
	if (fd < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "netlink_sock error: %d\n", fd);
		return fd;
	}

	if (af == FLUX_AF_INET)
		addr_sz = 4;
	else if (af == FLUX_AF_INET6)
		addr_sz = 16;
	else {
		FLUX_LOG(FLUX_LOG_ERR, "Bad address family: %d\n", af);
		return -1;
	}

	if (cmd != FLUX_RTM_DELROUTE) {
		req.r.rtm_protocol = FLUX_RTPROT_BOOT;
		req.r.rtm_scope = FLUX_RT_SCOPE_UNIVERSE;
		req.r.rtm_type = FLUX_RTN_UNICAST;
	}

	if (gwaddr)
		addattr_l(&req.n, sizeof(req), FLUX_RTA_GATEWAY, gwaddr,
			  addr_sz);

	if (af == FLUX_AF_INET && route_addr) {
		unsigned int netaddr = *(unsigned int *)route_addr;

		netaddr = ntohl(netaddr);
		netaddr = (netaddr >> (32 - route_masklen));
		netaddr = (netaddr << (32 - route_masklen));
		netaddr = htonl(netaddr);
		*(unsigned int *)route_addr = netaddr;
		req.r.rtm_dst_len = route_masklen;
		addattr_l(&req.n, sizeof(req), FLUX_RTA_DST, route_addr,
			  addr_sz);
	}

	if (af == FLUX_AF_INET6 && route_addr) {
		struct flux_in6_addr netaddr =
			*(struct flux_in6_addr *)route_addr;
		int rmbyte = route_masklen / 8;
		int rmbit = route_masklen % 8;

		for (i = 0; i < rmbyte; i++)
			netaddr.in6_u.u6_addr8[15 - i] = 0;
		netaddr.in6_u.u6_addr8[15 - rmbyte] =
			(netaddr.in6_u.u6_addr8[15 - rmbyte] >> rmbit);
		netaddr.in6_u.u6_addr8[15 - rmbyte] =
			(netaddr.in6_u.u6_addr8[15 - rmbyte] << rmbit);
		*(struct flux_in6_addr *)route_addr = netaddr;
		req.r.rtm_dst_len = route_masklen;
		addattr_l(&req.n, sizeof(req), FLUX_RTA_DST, route_addr,
			  addr_sz);
	}

	if (ifindex != FLUX_RT_TABLE_MAIN) {
		if (af == FLUX_AF_INET)
			req.r.rtm_table = ifindex * 2;
		else if (af == FLUX_AF_INET6)
			req.r.rtm_table = ifindex * 2 + 1;
		addattr_l(&req.n, sizeof(req), FLUX_RTA_OIF, &ifindex, addr_sz);
	}
	err = rtnl_talk(fd, &req.n);
	flux_sys_close(fd);
	return err;
}

int flux_if_add_linklocal(int ifindex, int af, void *addr, int netprefix_len)
{
	return iproute_modify(FLUX_RTM_NEWROUTE,
			      FLUX_NLM_F_CREATE | FLUX_NLM_F_EXCL, ifindex, af,
			      addr, netprefix_len, NULL);
}

int flux_if_add_gateway(int ifindex, int af, void *gwaddr)
{
	return iproute_modify(FLUX_RTM_NEWROUTE,
			      FLUX_NLM_F_CREATE | FLUX_NLM_F_EXCL, ifindex, af,
			      NULL, 0, gwaddr);
}

int flux_add_gateway(int af, void *gwaddr)
{
	return iproute_modify(FLUX_RTM_NEWROUTE,
			      FLUX_NLM_F_CREATE | FLUX_NLM_F_EXCL,
			      FLUX_RT_TABLE_MAIN, af, NULL, 0, gwaddr);
}

static int iprule_modify(int cmd, int ifindex, int af, void *saddr)
{
	struct {
		struct flux_nlmsghdr n;
		struct flux_rtmsg r;
		char buf[1024];
	} req = {
		.n.nlmsg_type = cmd,
		.n.nlmsg_len = FLUX_NLMSG_LENGTH(sizeof(struct flux_rtmsg)),
		.n.nlmsg_flags = FLUX_NLM_F_REQUEST,
		.r.rtm_protocol = FLUX_RTPROT_BOOT,
		.r.rtm_scope = FLUX_RT_SCOPE_UNIVERSE,
		.r.rtm_family = af,
		.r.rtm_type = FLUX_RTN_UNSPEC,
	};
	int fd, err;
	int addr_sz;

	if (af == FLUX_AF_INET)
		addr_sz = 4;
	else if (af == FLUX_AF_INET6)
		addr_sz = 16;
	else {
		FLUX_LOG(FLUX_LOG_ERR, "Bad address family: %d\n", af);
		return -1;
	}

	fd = netlink_sock(0);
	if (fd < 0)
		return fd;

	if (cmd == FLUX_RTM_NEWRULE) {
		req.n.nlmsg_flags |= FLUX_NLM_F_CREATE | FLUX_NLM_F_EXCL;
		req.r.rtm_type = FLUX_RTN_UNICAST;
	}

	// set from address
	req.r.rtm_src_len = 8 * addr_sz;
	addattr_l(&req.n, sizeof(req), FLUX_FRA_SRC, saddr, addr_sz);

	// use ifindex as table id
	if (af == FLUX_AF_INET)
		req.r.rtm_table = ifindex * 2;
	else if (af == FLUX_AF_INET6)
		req.r.rtm_table = ifindex * 2 + 1;

	err = rtnl_talk(fd, &req.n);

	flux_sys_close(fd);
	return err;
}

int flux_if_add_rule_from_saddr(int ifindex, int af, void *saddr)
{
	return iprule_modify(FLUX_RTM_NEWRULE, ifindex, af, saddr);
}

static int flux_if_dump(const char *ifname, int af, int type,
		       int (*handler)(struct flux_sockaddr_nl *,
				      struct flux_nlmsghdr *, void *))
{
	int ifindex, fd, ret;
	struct {
		struct flux_nlmsghdr nlmsg_info;
		struct flux_ifaddrmsg ifaddrmsg_info;
	} req;

	ifindex = flux_ifname_to_ifindex(ifname);
	if (ifindex < 0)
		return ifindex;

	fd = netlink_sock(1 << (FLUX_RTNLGRP_IPV4_IFADDR - 1));
	if (fd < 0)
		return fd;

	memset(&req, 0, sizeof(req));
	req.nlmsg_info.nlmsg_len =
		FLUX_NLMSG_LENGTH(sizeof(struct flux_ifaddrmsg));
	req.nlmsg_info.nlmsg_flags = FLUX_NLM_F_REQUEST | FLUX_NLM_F_DUMP;
	req.nlmsg_info.nlmsg_type = type;
	req.ifaddrmsg_info.ifa_family = af;
	req.ifaddrmsg_info.ifa_index = ifindex;

	ret = flux_sys_send(fd, &req, req.nlmsg_info.nlmsg_len, 0);
	if (ret < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "flux_sys_send failed: %d\n", ret);
		return ret;
	}

	ret = rtnl_listen(fd, handler, (void *)ifname);
	flux_sys_close(fd);
	return ret;
}

static int dump_ipv4_addr(struct flux_sockaddr_nl *nladdr,
			  struct flux_nlmsghdr *n, void *arg)
{
	struct flux_ifaddrmsg *ifa = FLUX_NLMSG_DATA(n);
	struct flux_rtattr *rta_tb[FLUX_IFA_MAX + 1];
	int len = n->nlmsg_len;
	const char *ifname = (const char *)arg;

	if (n->nlmsg_type != FLUX_RTM_NEWADDR)
		return 1;

	len -= FLUX_NLMSG_LENGTH(sizeof(*ifa));
	if (len < 0) {
		FLUX_LOG(FLUX_LOG_ERR, "BUG: wrong nlmsg len %d\n", len);
		return -1;
	}

	parse_rtattr(rta_tb, FLUX_IFA_MAX, FLUX_IFA_RTA(ifa),
		     n->nlmsg_len - FLUX_NLMSG_LENGTH(sizeof(*ifa)));

	if (ifa->ifa_family != FLUX_AF_INET)
		return 1;

	if (rta_tb[FLUX_IFA_ADDRESS]) {
		char addr_str[INET_ADDRSTRLEN];

		inet_ntop(FLUX_AF_INET, FLUX_RTA_DATA(rta_tb[FLUX_IFA_ADDRESS]),
			  addr_str, sizeof(addr_str));

		FLUX_LOG(FLUX_LOG_INFO, "%s: ifindex=%d addr=%s/%d\n", ifname,
			ifa->ifa_index, addr_str, ifa->ifa_prefixlen);

		return 0;
	}

	return 1;
}

int flux_if_dump_ipv4_addr(const char *ifname)
{
	return flux_if_dump(ifname, FLUX_AF_INET, FLUX_RTM_GETADDR,
			   dump_ipv4_addr);
}

static int dump_ipv4_rules(struct flux_sockaddr_nl *nladdr,
			   struct flux_nlmsghdr *n, void *arg)
{
	struct flux_fib_rule_hdr *frh = FLUX_NLMSG_DATA(n);
	struct flux_rtattr *tb[FLUX_FRA_MAX + 1];
	char buf[64], buf2[1024];

	memset(tb, 0, sizeof(tb));
#define FLUX_FRA_RTA(f)                          \
	((struct flux_rtattr *)(((char *)(f)) +  \
			       FLUX_NLMSG_ALIGN( \
				       sizeof(struct flux_fib_rule_hdr))))
	parse_rtattr(tb, FLUX_FRA_MAX, FLUX_FRA_RTA(frh),
		     FLUX_NLMSG_PAYLOAD(n, sizeof(*frh)));

	sprintf(buf2, "Rule: ");

	if (frh->family != FLUX_AF_INET)
		return 0;

	if (tb[FLUX_FRA_SRC]) {
		struct flux_in_addr *src = FLUX_RTA_DATA(tb[FLUX_FRA_SRC]);
		inet_ntop(FLUX_AF_INET, src, buf, sizeof(buf));
		sprintf(buf2 + strlen(buf2), "src %s/%u ", buf, frh->src_len);
	}

	if (tb[FLUX_FRA_DST]) {
		struct flux_in_addr *dst = FLUX_RTA_DATA(tb[FLUX_FRA_DST]);
		inet_ntop(FLUX_AF_INET, dst, buf, sizeof(buf));
		sprintf(buf2 + strlen(buf2), "dst %s/%u ", buf, frh->dst_len);
	}

	if (tb[FLUX_FRA_GOTO]) {
		sprintf(buf2 + strlen(buf2), "goto %u ",
			*(int *)FLUX_RTA_DATA(tb[FLUX_FRA_GOTO]));
	}

	if (frh->tos)
		sprintf(buf2 + strlen(buf2), "tos 0x%x ", frh->tos);

	if (tb[FLUX_FRA_TABLE]) {
		sprintf(buf2 + strlen(buf2), "table %u ",
			*(int *)FLUX_RTA_DATA(tb[FLUX_FRA_TABLE]));
	} else if (frh->table)
		sprintf(buf2 + strlen(buf2), "table %u ", frh->table);

	if (tb[FLUX_FRA_PRIORITY]) {
		sprintf(buf2 + strlen(buf2), "priority %u ",
			*(int *)FLUX_RTA_DATA(tb[FLUX_FRA_PRIORITY]));
	}

	if (tb[FLUX_FRA_FWMARK]) {
		sprintf(buf2 + strlen(buf2), "fwmark 0x%x ",
			*(int *)FLUX_RTA_DATA(tb[FLUX_FRA_FWMARK]));
	}

	FLUX_LOG(FLUX_LOG_INFO, "%s\n", buf2);
	return 0;
}

int flux_if_dump_ipv4_rules(const char *ifname)
{
	return flux_if_dump(ifname, FLUX_AF_INET, FLUX_RTM_GETRULE,
			   dump_ipv4_rules);
}
