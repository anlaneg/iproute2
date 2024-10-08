/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * rtm_map.c
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rt_names.h"
#include "utils.h"

char *rtnl_rtntype_n2a(int id, char *buf, int len)
{

	if (numeric) {
		snprintf(buf, len, "%d", id);
		return buf;
	}

	switch (id) {
	case RTN_UNSPEC:
		return "none";
	case RTN_UNICAST:
		return "unicast";
	case RTN_LOCAL:
		return "local";
	case RTN_BROADCAST:
		return "broadcast";
	case RTN_ANYCAST:
		return "anycast";
	case RTN_MULTICAST:
		return "multicast";
	case RTN_BLACKHOLE:
		return "blackhole";
	case RTN_UNREACHABLE:
		return "unreachable";
	case RTN_PROHIBIT:
		return "prohibit";
	case RTN_THROW:
		return "throw";
	case RTN_NAT:
		return "nat";
	case RTN_XRESOLVE:
		return "xresolve";
	default:
		snprintf(buf, len, "%d", id);
		return buf;
	}
}

//路由类型
int rtnl_rtntype_a2n(int *id, char *arg)
{
	char *end;
	unsigned long res;

	if (strcmp(arg, "local") == 0)
		res = RTN_LOCAL;
	else if (strcmp(arg, "nat") == 0)
		res = RTN_NAT;
	else if (matches(arg, "broadcast") == 0 ||
		 strcmp(arg, "brd") == 0)
		res = RTN_BROADCAST;
	else if (matches(arg, "anycast") == 0)
		res = RTN_ANYCAST;
	else if (matches(arg, "multicast") == 0)
		res = RTN_MULTICAST;
	else if (matches(arg, "prohibit") == 0)
		res = RTN_PROHIBIT;
	else if (matches(arg, "unreachable") == 0)
		res = RTN_UNREACHABLE;
	else if (matches(arg, "blackhole") == 0)
		res = RTN_BLACKHOLE;
	else if (matches(arg, "xresolve") == 0)
		res = RTN_XRESOLVE;
	else if (matches(arg, "unicast") == 0)
		res = RTN_UNICAST;
	else if (strcmp(arg, "throw") == 0)
		res = RTN_THROW;
	else {
		res = strtoul(arg, &end, 0);
		if (!end || end == arg || *end || res > 255)
			return -1;
	}
	*id = res;
	return 0;
}

static int get_rt_realms(__u32 *realms, char *arg)
{
	__u32 realm = 0;
	char *p = strchr(arg, '/');

	*realms = 0;
	if (p) {
	    /*包含'/'符，将其设置为‘\0'*/
		*p = 0;
		/*将前半部分转换为数字*/
		if (rtnl_rtrealm_a2n(realms, arg)) {
			*p = '/';
			return -1;
		}
		*realms <<= 16;
		/*还原'/'符号*/
		*p = '/';
		/*使后半部分参数指到'/'之后*/
		arg = p+1;
	}

	/*将arg转换为数字*/
	if (*arg && rtnl_rtrealm_a2n(&realm, arg))
		return -1;
	/*合并realm*/
	*realms |= realm;
	return 0;
}

int get_rt_realms_or_raw(__u32 *realms, char *arg)
{
	if (!get_rt_realms(realms, arg))
		return 0;

	/*将args转换为无符号整数*/
	return get_unsigned(realms, arg, 0);
}
