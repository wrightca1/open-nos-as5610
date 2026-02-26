/*
 * Netlink listener — RTNETLINK for link, route, neigh, addr.
 * Dispatches to SDK (port enable, L3 intf, egress, route, L2).
 */
#include "bcm56846.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if.h>
#include <linux/if_addr.h>
#include <linux/neighbour.h>

#define NETLINK_BUF_SIZE 65536
#define RTA_TB_SIZE 32
#define NDA_TB_SIZE 32
#define MAX_IFINDEX 512
#define NEIGH_CACHE_SIZE 256
#define MAX_PORTS 56

#ifndef NDA_RTA
#define NDA_RTA(r) ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#endif

static int netlink_fd = -1;
static int netlink_unit = 0;
static volatile int netlink_running = 1;

/* ifindex -> BCM port (1-based); -1 unknown */
static int ifindex_to_port[MAX_IFINDEX];
/* port (1-based) -> L3 intf_id from l3_intf_create */
static int port_to_intf_id[MAX_PORTS];

struct neigh_entry {
	uint32_t ip;
	int ifindex;
	uint8_t mac[6];
};
static struct neigh_entry neigh_cache[NEIGH_CACHE_SIZE];
static int neigh_cache_count;

static void parse_rtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{
	memset(tb, 0, sizeof(tb[0]) * (size_t)max);
	while (RTA_OK(rta, len)) {
		if (rta->rta_type < (unsigned int)max)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta, len);
	}
}

/* Parse ifname "swpN" -> BCM port (1-based); return 0 if not a switch port */
static int ifname_to_port(const char *ifname, int *port)
{
	unsigned int n;
	if (!ifname || strncmp(ifname, "swp", 3) != 0)
		return -1;
	if (sscanf(ifname + 3, "%u", &n) != 1 || n == 0 || n > 56)
		return -1;
	*port = (int)n;
	return 0;
}

static void neigh_cache_set(uint32_t ip, int ifindex, const uint8_t *mac)
{
	int i;
	for (i = 0; i < neigh_cache_count; i++) {
		if (neigh_cache[i].ip == ip && neigh_cache[i].ifindex == ifindex) {
			memcpy(neigh_cache[i].mac, mac, 6);
			return;
		}
	}
	if (neigh_cache_count >= NEIGH_CACHE_SIZE)
		return;
	neigh_cache[neigh_cache_count].ip = ip;
	neigh_cache[neigh_cache_count].ifindex = ifindex;
	memcpy(neigh_cache[neigh_cache_count].mac, mac, 6);
	neigh_cache_count++;
}

static int neigh_cache_get(uint32_t ip, int ifindex, uint8_t *mac)
{
	int i;
	for (i = 0; i < neigh_cache_count; i++) {
		if (neigh_cache[i].ip == ip && neigh_cache[i].ifindex == ifindex) {
			memcpy(mac, neigh_cache[i].mac, 6);
			return 0;
		}
	}
	return -1;
}

static void neigh_cache_remove(uint32_t ip, int ifindex)
{
	int i;
	for (i = 0; i < neigh_cache_count; i++) {
		if (neigh_cache[i].ip == ip && neigh_cache[i].ifindex == ifindex) {
			memmove(&neigh_cache[i], &neigh_cache[i + 1],
				(size_t)(neigh_cache_count - 1 - i) * sizeof(neigh_cache[0]));
			neigh_cache_count--;
			return;
		}
	}
}

static void handle_link(struct nlmsghdr *nlh)
{
	struct ifinfomsg *ifi;
	struct rtattr *tb[RTA_TB_SIZE];
	int len, port, up;

	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifi)))
		return;
	ifi = NLMSG_DATA(nlh);
	len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
	parse_rtattr(tb, RTA_TB_SIZE, IFLA_RTA(ifi), len);
	if (nlh->nlmsg_type == RTM_DELLINK) {
		if ((unsigned int)ifi->ifi_index < MAX_IFINDEX)
			ifindex_to_port[ifi->ifi_index] = -1;
		return;
	}
	if (tb[IFLA_IFNAME]) {
		const char *name = (const char *)RTA_DATA(tb[IFLA_IFNAME]);
		if (ifname_to_port(name, &port) == 0) {
			if ((unsigned int)ifi->ifi_index < MAX_IFINDEX)
				ifindex_to_port[ifi->ifi_index] = port;
			up = (ifi->ifi_flags & IFF_UP) ? 1 : 0;
			bcm56846_port_enable_set(netlink_unit, port, up);
		}
	}
}

static void handle_addr(struct nlmsghdr *nlh)
{
	struct ifaddrmsg *ifa;
	struct rtattr *tb[RTA_TB_SIZE];
	int len, port, intf_id;
	uint8_t mac[6];

	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa)))
		return;
	ifa = NLMSG_DATA(nlh);
	len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));
	parse_rtattr(tb, RTA_TB_SIZE, IFA_RTA(ifa), len);
	if (!tb[IFA_ADDRESS])
		return;
	if ((unsigned int)ifa->ifa_index >= MAX_IFINDEX)
		return;
	port = ifindex_to_port[ifa->ifa_index];
	if (port <= 0 || port >= MAX_PORTS)
		return;

	/* Synthetic port MAC for EGR_L3_INTF: 02:00:00:00:00:XX */
	memset(mac, 0, 6);
	mac[0] = 0x02;
	mac[5] = (uint8_t)port;

	if (nlh->nlmsg_type == RTM_NEWADDR) {
		if (bcm56846_l3_intf_create(netlink_unit, mac, 0, &intf_id) == 0)
			port_to_intf_id[port] = intf_id;
	} else {
		if (port_to_intf_id[port] != 0) {
			bcm56846_l3_intf_destroy(netlink_unit, port_to_intf_id[port]);
			port_to_intf_id[port] = 0;
		}
	}
}

static void handle_neigh(struct nlmsghdr *nlh)
{
	struct ndmsg *ndm;
	struct rtattr *tb[NDA_TB_SIZE];
	int len, port;
	uint8_t *lladdr = NULL;
	uint8_t mac[6];
	uint16_t vid = 0;
	uint32_t dst_ip = 0;

	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*ndm)))
		return;
	ndm = NLMSG_DATA(nlh);
	if (ndm->ndm_family != AF_INET)
		return;
	len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ndm));
	parse_rtattr(tb, NDA_TB_SIZE, NDA_RTA(ndm), len);
	if (tb[NDA_LLADDR] && RTA_PAYLOAD(tb[NDA_LLADDR]) >= 6)
		lladdr = RTA_DATA(tb[NDA_LLADDR]);
	if (tb[NDA_DST])
		memcpy(&dst_ip, RTA_DATA(tb[NDA_DST]), 4);
	if (tb[NDA_VLAN])
		vid = *(uint16_t *)RTA_DATA(tb[NDA_VLAN]);

	if ((unsigned int)ndm->ndm_ifindex >= MAX_IFINDEX)
		return;
	port = ifindex_to_port[ndm->ndm_ifindex];
	if (port <= 0)
		return;

	if (nlh->nlmsg_type == RTM_NEWNEIGH) {
		if (!lladdr)
			return;
		{
			bcm56846_l2_addr_t l2;
			memcpy(l2.mac, lladdr, 6);
			l2.vid = vid;
			l2.port = port;
			l2.static_entry = 1;
			bcm56846_l2_addr_add(netlink_unit, &l2);
		}
		neigh_cache_set(dst_ip, ndm->ndm_ifindex, lladdr);
	} else {
		if (lladdr)
			bcm56846_l2_addr_delete(netlink_unit, lladdr, vid);
		else if (dst_ip && neigh_cache_get(dst_ip, ndm->ndm_ifindex, mac) == 0)
			bcm56846_l2_addr_delete(netlink_unit, mac, vid);
		neigh_cache_remove(dst_ip, ndm->ndm_ifindex);
	}
}

static void handle_route(struct nlmsghdr *nlh)
{
	struct rtmsg *rtm;
	struct rtattr *tb[RTA_TB_SIZE];
	int len, port, egress_id;
	uint32_t dst = 0, gateway = 0, mask;
	bcm56846_l3_egress_t egr;
	bcm56846_l3_route_t route;

	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*rtm)))
		return;
	rtm = NLMSG_DATA(nlh);
	if (rtm->rtm_family != AF_INET || rtm->rtm_type != RTN_UNICAST)
		return;
	len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm));
	parse_rtattr(tb, RTA_TB_SIZE, RTM_RTA(rtm), len);
	if (tb[RTA_DST])
		memcpy(&dst, RTA_DATA(tb[RTA_DST]), 4);
	if (rtm->rtm_dst_len > 0 && rtm->rtm_dst_len <= 32)
		mask = (uint32_t)(0xFFFFFFFFU << (32 - rtm->rtm_dst_len));
	else
		mask = 0;

	if (nlh->nlmsg_type == RTM_DELROUTE) {
		route.prefix = dst;
		route.prefix_len = (int)rtm->rtm_dst_len;
		route.egress_id = 0;
		route.is_ipv6 = 0;
		bcm56846_l3_route_delete(netlink_unit, &route);
		return;
	}

	/* RTM_NEWROUTE */
	{
		int oif = 0;
		if (tb[RTA_GATEWAY])
			memcpy(&gateway, RTA_DATA(tb[RTA_GATEWAY]), 4);
		if (tb[RTA_OIF])
			oif = *(int *)RTA_DATA(tb[RTA_OIF]);

		if ((unsigned int)oif >= MAX_IFINDEX)
			return;
		port = ifindex_to_port[oif];
		if (port <= 0)
			return;

		memset(&egr, 0, sizeof(egr));
		egr.port = port;
		egr.vid = 0;
		egr.intf_id = (port < MAX_PORTS) ? port_to_intf_id[port] : 0;
		neigh_cache_get(gateway, oif, egr.mac);

		if (bcm56846_l3_egress_create(netlink_unit, &egr, &egress_id) != 0)
			return;
		memset(&route, 0, sizeof(route));
		route.prefix = dst;
		route.prefix_len = (int)rtm->rtm_dst_len;
		route.egress_id = egress_id;
		route.is_ipv6 = 0;
		bcm56846_l3_route_add(netlink_unit, &route);
	}
}

static void dispatch(struct nlmsghdr *nlh)
{
	switch (nlh->nlmsg_type) {
	case RTM_NEWLINK:
	case RTM_DELLINK:
		handle_link(nlh);
		break;
	case RTM_NEWADDR:
	case RTM_DELADDR:
		handle_addr(nlh);
		break;
	case RTM_NEWROUTE:
	case RTM_DELROUTE:
		handle_route(nlh);
		break;
	case RTM_NEWNEIGH:
	case RTM_DELNEIGH:
		handle_neigh(nlh);
		break;
	default:
		break;
	}
}

void *netlink_thread(void *arg)
{
	int unit = *(int *)arg;
	char *buf;
	struct nlmsghdr *nlh;
	int len;

	netlink_unit = unit;
	memset(ifindex_to_port, 0xff, sizeof(ifindex_to_port));
	memset(port_to_intf_id, 0, sizeof(port_to_intf_id));
	neigh_cache_count = 0;

	buf = malloc(NETLINK_BUF_SIZE);
	if (!buf)
		return NULL;

	netlink_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (netlink_fd < 0) {
		fprintf(stderr, "netlink: socket failed: %d\n", errno);
		free(buf);
		return NULL;
	}

	{
		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
		sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE |
			       RTMGRP_NEIGH | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
		if (bind(netlink_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			fprintf(stderr, "netlink: bind failed: %d\n", errno);
			close(netlink_fd);
			netlink_fd = -1;
			free(buf);
			return NULL;
		}
	}

	while (netlink_running) {
		len = recv(netlink_fd, buf, NETLINK_BUF_SIZE, 0);
		if (len <= 0) {
			if (len < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			break;
		}
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
			if (nlh->nlmsg_type == NLMSG_DONE || nlh->nlmsg_type == NLMSG_ERROR)
				continue;
			dispatch(nlh);
		}
	}

	close(netlink_fd);
	netlink_fd = -1;
	free(buf);
	return NULL;
}

void netlink_stop(void)
{
	netlink_running = 0;
	if (netlink_fd >= 0)
		shutdown(netlink_fd, SHUT_RDWR);
}
