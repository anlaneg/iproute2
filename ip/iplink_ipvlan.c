/* SPDX-License-Identifier: GPL-2.0-or-later */
/* iplink_ipvlan.c	IPVLAN/IPVTAP device support
 *
 * Authors:     Mahesh Bandewar <maheshb@google.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/if_link.h>

#include "rt_names.h"
#include "utils.h"
#include "ip_common.h"

//ipvaln 帮助信息
static void print_explain(struct link_util *lu, FILE *f)
{
	fprintf(f,
		"Usage: ... %s [ mode MODE ] [ FLAGS ]\n"
		"\n"
		"MODE: l3 | l3s | l2\n"
		"FLAGS: bridge | private | vepa\n"
		"(first values are the defaults if nothing is specified).\n",
		lu->id);
}

static int ipvlan_parse_opt(struct link_util *lu, int argc, char **argv,
			    struct nlmsghdr *n)
{
	__u16 flags = 0;
	bool mflag_given = false;

	while (argc > 0) {
		if (matches(*argv, "mode") == 0) {
			//mode解析
			__u16 mode = 0;

			NEXT_ARG();

			if (strcmp(*argv, "l2") == 0)
				mode = IPVLAN_MODE_L2;
			else if (strcmp(*argv, "l3") == 0)
				mode = IPVLAN_MODE_L3;
			else if (strcmp(*argv, "l3s") == 0)
				mode = IPVLAN_MODE_L3S;
			else {
				fprintf(stderr, "Error: argument of \"mode\" must be either \"l2\", \"l3\" or \"l3s\"\n");
				return -1;
			}
			//添加ipvlan_mode
			addattr16(n, 1024, IFLA_IPVLAN_MODE, mode);
		} else if (matches(*argv, "private") == 0 && !mflag_given) {
			flags |= IPVLAN_F_PRIVATE;
			mflag_given = true;
		} else if (matches(*argv, "vepa") == 0 && !mflag_given) {
			flags |= IPVLAN_F_VEPA;
			mflag_given = true;
		} else if (matches(*argv, "bridge") == 0 && !mflag_given) {
			/*默认使用桥模式*/
			mflag_given = true;
		} else if (matches(*argv, "help") == 0) {
			print_explain(lu, stderr);
			return -1;
		} else {
			fprintf(stderr, "%s: unknown option \"%s\"?\n",
				lu->id, *argv);
			print_explain(lu, stderr);
			return -1;
		}
		argc--;
		argv++;
	}
	//添加private,vpea,bridge各模式
	addattr16(n, 1024, IFLA_IPVLAN_FLAGS, flags);

	return 0;
}

static void ipvlan_print_opt(struct link_util *lu, FILE *f, struct rtattr *tb[])
{

	if (!tb)
		return;

	if (tb[IFLA_IPVLAN_MODE]) {
		//有ipvlan_mode,输出mode字符串
		if (RTA_PAYLOAD(tb[IFLA_IPVLAN_MODE]) == sizeof(__u16)) {
			__u16 mode = rta_getattr_u16(tb[IFLA_IPVLAN_MODE]);
			const char *mode_str = mode == IPVLAN_MODE_L2 ? "l2" :
				mode == IPVLAN_MODE_L3 ? "l3" :
				mode == IPVLAN_MODE_L3S ? "l3s" : "unknown";

			print_string(PRINT_ANY, "mode", " mode %s ", mode_str);
		}
	}
	if (tb[IFLA_IPVLAN_FLAGS]) {
		//有ipvlan_flags,输出不同的模式，默认bridge
		if (RTA_PAYLOAD(tb[IFLA_IPVLAN_FLAGS]) == sizeof(__u16)) {
			__u16 flags = rta_getattr_u16(tb[IFLA_IPVLAN_FLAGS]);

			if (flags & IPVLAN_F_PRIVATE)
				print_bool(PRINT_ANY, "private", "private ",
					   true);
			else if (flags & IPVLAN_F_VEPA)
				print_bool(PRINT_ANY, "vepa", "vepa ",
					   true);
			else
				print_bool(PRINT_ANY, "bridge", "bridge ",
					   true);
		}
	}
}

//显示ipvaln帮助信息
static void ipvlan_print_help(struct link_util *lu, int argc, char **argv,
			      FILE *f)
{
	print_explain(lu, f);
}

struct link_util ipvlan_link_util = {
	.id		= "ipvlan",
	.maxattr	= IFLA_IPVLAN_MAX,
	.parse_opt	= ipvlan_parse_opt,
	.print_opt	= ipvlan_print_opt,
	.print_help	= ipvlan_print_help,
};

struct link_util ipvtap_link_util = {
	.id		= "ipvtap",
	.maxattr	= IFLA_IPVLAN_MAX,
	.parse_opt	= ipvlan_parse_opt,
	.print_opt	= ipvlan_print_opt,
	.print_help	= ipvlan_print_help,
};
