/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * m_pedit.h		generic packet editor actions module
 *
 *
 * Authors:  J Hadi Salim (hadi@cyberus.ca)
 *
 */

#ifndef _ACT_PEDIT_H_
#define _ACT_PEDIT_H_ 1

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
#include <linux/tc_act/tc_pedit.h>

#define MAX_OFFS 128

#define TIPV4 1
#define TIPV6 2
#define TINT 3
#define TU32 4
#define TMAC 5

#define RU32 0xFFFFFFFF
#define RU16 0xFFFF
#define RU8 0xFF

#define PEDITKINDSIZ 16

enum m_pedit_flags {
	PEDIT_ALLOW_DEC = 1<<0,
};

struct m_pedit_key {
	//掩码
	__u32           mask;  /* AND */
	//目标值
	__u32           val;   /*XOR */
	//字段的在key中的偏移量
	__u32           off;  /*offset */
	/**
	 * 支持at子句
	 * at AT offmask MASK shift SHIFT
		This is an optional part of RAW_OP which allows to have a variable OFFSET
		depending on packet data at offset AT, which is binary ANDed with MASK and
		right-shifted by SHIFT before adding it to OFFSET.
	 */
	__u32           at;
	__u32           offmask;
	__u32           shift;

	enum pedit_header_type htype;//字段所在的header类型
	enum pedit_cmd cmd;//修改方式,add/modify
};

struct m_pedit_key_ex {
	//修改的头部类型
	enum pedit_header_type htype;
	enum pedit_cmd cmd;
};

struct m_pedit_sel {
	struct tc_pedit_sel sel;
	struct tc_pedit_key keys[MAX_OFFS];
	struct m_pedit_key_ex keys_ex[MAX_OFFS];
	bool extended;
};

struct m_pedit_util {
	struct m_pedit_util *next;
	char    id[PEDITKINDSIZ];
	int     (*parse_peopt)(int *argc_p, char ***argv_p,
			       struct m_pedit_sel *sel,
			       struct m_pedit_key *tkey);
};

int parse_cmd(int *argc_p, char ***argv_p, __u32 len, int type,
	      __u32 retain,
	      struct m_pedit_sel *sel, struct m_pedit_key *tkey, int flags);
#endif
