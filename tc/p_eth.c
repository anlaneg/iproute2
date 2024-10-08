/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * m_pedit_eth.c	packet editor: ETH header
 *
 *
 * Authors:  Amir Vadai (amir@vadai.me)
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

static int
parse_eth(int *argc_p, char ***argv_p,
	  struct m_pedit_sel *sel, struct m_pedit_key *tkey)
{
	int res = -1;
	int argc = *argc_p;
	char **argv = *argv_p;

	if (argc < 2)
		return -1;

	if (!sel->extended)
		return -1;

	tkey->htype = TCA_PEDIT_KEY_EX_HDR_TYPE_ETH;

	if (strcmp(*argv, "type") == 0) {
		NEXT_ARG();
		tkey->off = 12;
		res = parse_cmd(&argc, &argv, 2, TU32, RU16, sel, tkey, 0);
		goto done;
	}

	if (strcmp(*argv, "dst") == 0) {
		NEXT_ARG();
		tkey->off = 0;
		res = parse_cmd(&argc, &argv, 6, TMAC, RU32, sel, tkey, 0);
		goto done;
	}

	if (strcmp(*argv, "src") == 0) {
		NEXT_ARG();
		tkey->off = 6;
		res = parse_cmd(&argc, &argv, 6, TMAC, RU32, sel, tkey, 0);
		goto done;
	}

	return -1;

done:
	*argc_p = argc;
	*argv_p = argv;
	return res;
}

//以太头修改帮助函数
struct m_pedit_util p_pedit_eth = {
	.id = "eth",
	.parse_peopt = parse_eth,
};
