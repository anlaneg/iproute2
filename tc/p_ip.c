/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * p_ip.c		packet editor: IPV4 header
 *
 * Authors:  J Hadi Salim (hadi@cyberus.ca)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include "utils.h"
#include "tc_util.h"
#include "m_pedit.h"

//解析pedit mungle ip后面的命令行
static int
parse_ip(int *argc_p, char ***argv_p,
	 struct m_pedit_sel *sel, struct m_pedit_key *tkey/*出参，解析到的key*/)
{
	int res = -1;
	int argc = *argc_p;
	char **argv = *argv_p;

	if (argc < 2)
		return -1;

	tkey->htype = sel->extended ?
		TCA_PEDIT_KEY_EX_HDR_TYPE_IP4 :
		TCA_PEDIT_KEY_EX_HDR_TYPE_NETWORK;

	if (strcmp(*argv, "src") == 0) {
		//解析src ip地址
		NEXT_ARG();
		tkey->off = 12;
		res = parse_cmd(&argc, &argv, 4, TIPV4, RU32, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "dst") == 0) {
		//解析dst ip地址
		NEXT_ARG();
		tkey->off = 16;
		res = parse_cmd(&argc, &argv, 4, TIPV4, RU32, sel, tkey, 0);
		goto done;
	}
	/* jamal - look at these and make them either old or new
	** scheme given diffserv
	** don't forget the CE bit
	*/
	if (strcmp(*argv, "tos") == 0 || matches(*argv, "dsfield") == 0) {
		NEXT_ARG();
		tkey->off = 1;
		res = parse_cmd(&argc, &argv, 1, TU32, RU8, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "ihl") == 0) {
		NEXT_ARG();
		tkey->off = 0;
		res = parse_cmd(&argc, &argv, 1, TU32, 0x0f, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "ttl") == 0) {
		NEXT_ARG();
		tkey->off = 8;
		res = parse_cmd(&argc, &argv, 1, TU32, RU8, sel, tkey, PEDIT_ALLOW_DEC);
		goto done;
	}
	if (strcmp(*argv, "protocol") == 0) {
		NEXT_ARG();
		tkey->off = 9;
		res = parse_cmd(&argc, &argv, 1, TU32, RU8, sel, tkey, 0);
		goto done;
	}
	/* jamal - fix this */
	if (matches(*argv, "precedence") == 0) {
		NEXT_ARG();
		tkey->off = 1;
		res = parse_cmd(&argc, &argv, 1, TU32, RU8, sel, tkey, 0);
		goto done;
	}
	/* jamal - validate this at some point */
	if (strcmp(*argv, "nofrag") == 0) {
		NEXT_ARG();
		tkey->off = 6;
		res = parse_cmd(&argc, &argv, 1, TU32, 0x3F, sel, tkey, 0);
		goto done;
	}
	/* jamal - validate this at some point */
	if (strcmp(*argv, "firstfrag") == 0) {
		NEXT_ARG();
		tkey->off = 6;
		res = parse_cmd(&argc, &argv, 1, TU32, 0x1F, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "ce") == 0) {
		NEXT_ARG();
		tkey->off = 6;
		res = parse_cmd(&argc, &argv, 1, TU32, 0x80, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "df") == 0) {
		NEXT_ARG();
		tkey->off = 6;
		res = parse_cmd(&argc, &argv, 1, TU32, 0x40, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "mf") == 0) {
		NEXT_ARG();
		tkey->off = 6;
		res = parse_cmd(&argc, &argv, 1, TU32, 0x20, sel, tkey, 0);
		goto done;
	}

	if (sel->extended)
		return -1; /* fields located outside IP header should be
			    * addressed using the relevant header type in
			    * extended pedit kABI
			    */

	if (strcmp(*argv, "dport") == 0) {
		NEXT_ARG();
		tkey->off = 22;
		res = parse_cmd(&argc, &argv, 2, TU32, RU16, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "sport") == 0) {
		NEXT_ARG();
		tkey->off = 20;
		res = parse_cmd(&argc, &argv, 2, TU32, RU16, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "icmp_type") == 0) {
		NEXT_ARG();
		tkey->off = 20;
		res = parse_cmd(&argc, &argv, 1, TU32, RU8, sel, tkey, 0);
		goto done;
	}
	if (strcmp(*argv, "icmp_code") == 0) {
		NEXT_ARG();
		tkey->off = 20;
		res = parse_cmd(&argc, &argv, 1, TU32, RU8, sel, tkey, 0);
		goto done;
	}
	return -1;

done:
	*argc_p = argc;
	*argv_p = argv;
	return res;
}

//ip报文字段修改辅助函数
struct m_pedit_util p_pedit_ip = {
	.id = "ip",
	.parse_peopt = parse_ip,//解析ip后面的参数
};
