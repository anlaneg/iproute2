/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * link_veth.c	veth driver module
 *
 * Authors:	Pavel Emelianov <xemul@openvz.org>
 */

#include <string.h>
#include <net/if.h>
#include <linux/veth.h>

#include "utils.h"
#include "ip_common.h"

static void print_usage(FILE *f)
{
	printf("Usage: ip link <options> type veth [peer <options>]\n"
	       "To get <options> type 'ip link add help'\n");
}

static void usage(void)
{
	print_usage(stderr);
}

//veth参数解析，并填充lu
static int veth_parse_opt(struct link_util *lu, int argc, char **argv,
			  struct nlmsghdr *n)
{
	char *type = NULL;
	int err;
	struct rtattr *data;
	struct ifinfomsg *ifm, *peer_ifm;
	unsigned int ifi_flags, ifi_change, ifi_index;

	//首个字符串必须为peer
	if (strcmp(argv[0], "peer") != 0) {
		usage();
		return -1;
	}

	ifm = NLMSG_DATA(n);
	ifi_flags = ifm->ifi_flags;
	ifi_change = ifm->ifi_change;
	ifi_index = ifm->ifi_index;
	ifm->ifi_flags = 0;
	ifm->ifi_change = 0;
	ifm->ifi_index = 0;

	data = addattr_nest(n, 1024, VETH_INFO_PEER);

	n->nlmsg_len += sizeof(struct ifinfomsg);

	err = iplink_parse(argc - 1, argv + 1, (struct iplink_req *)n, &type);
	if (err < 0)
		return err;

	if (type)
		duparg("type", argv[err]);

	peer_ifm = RTA_DATA(data);
	peer_ifm->ifi_index = ifm->ifi_index;
	peer_ifm->ifi_flags = ifm->ifi_flags;
	peer_ifm->ifi_change = ifm->ifi_change;
	ifm->ifi_flags = ifi_flags;
	ifm->ifi_change = ifi_change;
	ifm->ifi_index = ifi_index;

	addattr_nest_end(n, data);
	return argc - 1 - err;
}

static void veth_print_help(struct link_util *lu, int argc, char **argv,
	FILE *f)
{
	print_usage(f);
}

struct link_util veth_link_util = {
	.id = "veth",
	.parse_opt = veth_parse_opt,
	.print_help = veth_print_help,
};
