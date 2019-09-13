/*
 * f_flower.c		Flower Classifier
 *
 *		This program is free software; you can distribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:     Jiri Pirko <jiri@resnulli.us>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <linux/limits.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tc_act/tc_vlan.h>
#include <linux/mpls.h>

#include "utils.h"
#include "tc_util.h"
#include "rt_names.h"

enum flower_matching_flags {
	FLOWER_IP_FLAGS,
};

enum flower_endpoint {
	FLOWER_ENDPOINT_SRC,
	FLOWER_ENDPOINT_DST
};

enum flower_icmp_field {
	FLOWER_ICMP_FIELD_TYPE,
	FLOWER_ICMP_FIELD_CODE
};

static void explain(void)
{
	fprintf(stderr,
		"Usage: ... flower	[ MATCH-LIST ] [ verbose ]\n"
		"			[ skip_sw | skip_hw ]\n"
		"			[ action ACTION-SPEC ] [ classid CLASSID ]\n"
		"\n"
		"Where: MATCH-LIST := [ MATCH-LIST ] MATCH\n"
		"       MATCH      := {	indev DEV-NAME |\n"
		"			vlan_id VID |\n"
		"			vlan_prio PRIORITY |\n"
		"			vlan_ethtype [ ipv4 | ipv6 | ETH-TYPE ] |\n"
		"			cvlan_id VID |\n"
		"			cvlan_prio PRIORITY |\n"
		"			cvlan_ethtype [ ipv4 | ipv6 | ETH-TYPE ] |\n"
		"			dst_mac MASKED-LLADDR |\n"
		"			src_mac MASKED-LLADDR |\n"
		"			ip_proto [tcp | udp | sctp | icmp | icmpv6 | IP-PROTO ] |\n"
		"			ip_tos MASKED-IP_TOS |\n"
		"			ip_ttl MASKED-IP_TTL |\n"
		"			mpls_label LABEL |\n"
		"			mpls_tc TC |\n"
		"			mpls_bos BOS |\n"
		"			mpls_ttl TTL |\n"
		"			dst_ip PREFIX |\n"
		"			src_ip PREFIX |\n"
		"			dst_port PORT-NUMBER |\n"
		"			src_port PORT-NUMBER |\n"
		"			tcp_flags MASKED-TCP_FLAGS |\n"
		"			type MASKED-ICMP-TYPE |\n"
		"			code MASKED-ICMP-CODE |\n"
		"			arp_tip IPV4-PREFIX |\n"
		"			arp_sip IPV4-PREFIX |\n"
		"			arp_op [ request | reply | OP ] |\n"
		"			arp_tha MASKED-LLADDR |\n"
		"			arp_sha MASKED-LLADDR |\n"
		"			enc_dst_ip [ IPV4-ADDR | IPV6-ADDR ] |\n"
		"			enc_src_ip [ IPV4-ADDR | IPV6-ADDR ] |\n"
		"			enc_key_id [ KEY-ID ] |\n"
		"			enc_tos MASKED-IP_TOS |\n"
		"			enc_ttl MASKED-IP_TTL |\n"
		"			geneve_opts MASKED-OPTIONS |\n"
		"			ip_flags IP-FLAGS | \n"
		"			enc_dst_port [ port_number ] }\n"
		"	FILTERID := X:Y:Z\n"
		"	MASKED_LLADDR := { LLADDR | LLADDR/MASK | LLADDR/BITS }\n"
		"	ACTION-SPEC := ... look at individual actions\n"
		"\n"
		"NOTE:	CLASSID, IP-PROTO are parsed as hexadecimal input.\n"
		"NOTE:	There can be only used one mask per one prio. If user needs\n"
		"	to specify different mask, he has to use different prio.\n");
}

static int flower_parse_eth_addr(char *str, int addr_type, int mask_type,
				 struct nlmsghdr *n)
{
	int ret, err = -1;
	char addr[ETH_ALEN], *slash;

	slash = strchr(str, '/');
	if (slash)
		*slash = '\0';

	//mac解析
	ret = ll_addr_a2n(addr, sizeof(addr), str);
	if (ret < 0)
		goto err;
	//存入mac地址到buffer
	addattr_l(n, MAX_MSG, addr_type, addr, sizeof(addr));

	if (slash) {
		//支持mac的掩码形式
		unsigned bits;

		if (!get_unsigned(&bits, slash + 1, 10)) {
			uint64_t mask;

			/* Extra 16 bit shift to push mac address into
			 * high bits of uint64_t
			 */
			mask = htonll(0xffffffffffffULL << (16 + 48 - bits));
			memcpy(addr, &mask, ETH_ALEN);
		} else {
			ret = ll_addr_a2n(addr, sizeof(addr), slash + 1);
			if (ret < 0)
				goto err;
		}
	} else {
		//全掩码形式
		memset(addr, 0xff, ETH_ALEN);
	}
	//存入掩码
	addattr_l(n, MAX_MSG, mask_type, addr, sizeof(addr));

	err = 0;
err:
	if (slash)
		*slash = '/';
	return err;
}

static bool eth_type_vlan(__be16 ethertype)
{
	return ethertype == htons(ETH_P_8021Q) ||
	       ethertype == htons(ETH_P_8021AD);
}

static int flower_parse_vlan_eth_type(char *str, __be16 eth_type, int type,
				      __be16 *p_vlan_eth_type,
				      struct nlmsghdr *n)
{
	__be16 vlan_eth_type;

	if (!eth_type_vlan(eth_type)) {
		fprintf(stderr, "Can't set \"%s\" if ethertype isn't 802.1Q or 802.1AD\n",
			type == TCA_FLOWER_KEY_VLAN_ETH_TYPE ? "vlan_ethtype" : "cvlan_ethtype");
		return -1;
	}

	if (ll_proto_a2n(&vlan_eth_type, str))
		invarg("invalid vlan_ethtype", str);
	addattr16(n, MAX_MSG, type, vlan_eth_type);
	*p_vlan_eth_type = vlan_eth_type;
	return 0;
}

struct flag_to_string {
	int flag;
	enum flower_matching_flags type;
	char *string;
};

static struct flag_to_string flags_str[] = {
	{ TCA_FLOWER_KEY_FLAGS_IS_FRAGMENT, FLOWER_IP_FLAGS, "frag" },
	{ TCA_FLOWER_KEY_FLAGS_FRAG_IS_FIRST, FLOWER_IP_FLAGS, "firstfrag" },
};

static int flower_parse_matching_flags(char *str,
				       enum flower_matching_flags type,
				       __u32 *mtf, __u32 *mtf_mask)
{
	char *token;
	bool no;
	bool found;
	int i;

	token = strtok(str, "/");

	while (token) {
		if (!strncmp(token, "no", 2)) {
			no = true;
			token += 2;
		} else
			no = false;

		found = false;
		for (i = 0; i < ARRAY_SIZE(flags_str); i++) {
			if (type != flags_str[i].type)
				continue;

			if (!strcmp(token, flags_str[i].string)) {
				if (no)
					*mtf &= ~flags_str[i].flag;
				else
					*mtf |= flags_str[i].flag;

				*mtf_mask |= flags_str[i].flag;
				found = true;
				break;
			}
		}
		if (!found)
			return -1;

		token = strtok(NULL, "/");
	}

	return 0;
}

//将str解析为ip,ipv6协议的protocol的协议字段
static int flower_parse_ip_proto(char *str, __be16 eth_type/*帧类型*/, int type,
				 __u8 *p_ip_proto, struct nlmsghdr *n)
{
	int ret;
	__u8 ip_proto;

	//当前仅支持ip,ipv6
	if (eth_type != htons(ETH_P_IP) && eth_type != htons(ETH_P_IPV6))
		goto err;

	//按协议名称映射协议编号
	if (matches(str, "tcp") == 0) {
		ip_proto = IPPROTO_TCP;
	} else if (matches(str, "udp") == 0) {
		ip_proto = IPPROTO_UDP;
	} else if (matches(str, "sctp") == 0) {
		ip_proto = IPPROTO_SCTP;
	} else if (matches(str, "icmp") == 0) {
		if (eth_type != htons(ETH_P_IP))
			goto err;
		ip_proto = IPPROTO_ICMP;
	} else if (matches(str, "icmpv6") == 0) {
		if (eth_type != htons(ETH_P_IPV6))
			goto err;
		ip_proto = IPPROTO_ICMPV6;
	} else {
		//采用数字指明的protocol
		ret = get_u8(&ip_proto, str, 16);
		if (ret)
			return -1;
	}
	//存入type及其对应的value(ip_proto)
	addattr8(n, MAX_MSG, type, ip_proto);
	*p_ip_proto = ip_proto;
	return 0;

err:
	fprintf(stderr, "Illegal \"eth_type\" for ip proto\n");
	return -1;
}

//解析ip地址，并填充address,netmask到netlink消息
static int __flower_parse_ip_addr(char *str, int family,
				  int addr4_type, int mask4_type,
				  int addr6_type, int mask6_type,
				  struct nlmsghdr *n)
{
	int ret;
	inet_prefix addr;
	int bits;
	int i;

	//解析前缀
	ret = get_prefix(&addr, str, family);
	if (ret)
		return -1;

	//协议族不相等，返回-1
	if (family && (addr.family != family)) {
		fprintf(stderr, "Illegal \"eth_type\" for ip address\n");
		return -1;
	}

	//填充address
	addattr_l(n, MAX_MSG, addr.family == AF_INET ? addr4_type : addr6_type,
		  addr.data, addr.bytelen);

	//构造掩码（设置bitlen外的bit为0）
	memset(addr.data, 0xff, addr.bytelen);
	bits = addr.bitlen;
	for (i = 0; i < addr.bytelen / 4; i++) {
		if (!bits) {
			addr.data[i] = 0;
		} else if (bits / 32 >= 1) {
			bits -= 32;
		} else {
			addr.data[i] <<= 32 - bits;
			addr.data[i] = htonl(addr.data[i]);
			bits = 0;
		}
	}

	//填充address的掩码
	addattr_l(n, MAX_MSG, addr.family == AF_INET ? mask4_type : mask6_type,
		  addr.data, addr.bytelen);

	return 0;
}

//解析并填充ip地址及其掩码
static int flower_parse_ip_addr(char *str, __be16 eth_type,
				int addr4_type/*如str为ipv4地址，则v4地址类型*/, int mask4_type/*如str为ipv4地址，则v4地址mask类型*/,
				int addr6_type/*如str为ipv6地址，则v6地址类型*/, int mask6_type/*如str为ipv6地址，则v6地址mask类型*/,
				struct nlmsghdr *n)
{
	int family;

	//确定协议族
	if (eth_type == htons(ETH_P_IP)) {
		family = AF_INET;
	} else if (eth_type == htons(ETH_P_IPV6)) {
		family = AF_INET6;
	} else if (!eth_type) {
		family = AF_UNSPEC;
	} else {
		return -1;
	}

	//解析并填充ip地址及其掩码
	return __flower_parse_ip_addr(str, family, addr4_type, mask4_type,
				      addr6_type, mask6_type, n);
}

static bool flower_eth_type_arp(__be16 eth_type)
{
	return eth_type == htons(ETH_P_ARP) || eth_type == htons(ETH_P_RARP);
}

static int flower_parse_arp_ip_addr(char *str, __be16 eth_type,
				    int addr_type, int mask_type,
				    struct nlmsghdr *n)
{
	if (!flower_eth_type_arp(eth_type))
		return -1;

	return __flower_parse_ip_addr(str, AF_INET, addr_type, mask_type,
				      TCA_FLOWER_UNSPEC, TCA_FLOWER_UNSPEC, n);
}

static int flower_parse_u8(char *str, int value_type, int mask_type,
			   int (*value_from_name)(const char *str,
						 __u8 *value),
			   bool (*value_validate)(__u8 value),
			   struct nlmsghdr *n)
{
	char *slash;
	int ret, err = -1;
	__u8 value, mask;

	slash = strchr(str, '/');
	if (slash)
		*slash = '\0';

	ret = value_from_name ? value_from_name(str, &value) : -1;
	if (ret < 0) {
		ret = get_u8(&value, str, 10);
		if (ret)
			goto err;
	}

	if (value_validate && !value_validate(value))
		goto err;

	if (slash) {
		ret = get_u8(&mask, slash + 1, 10);
		if (ret)
			goto err;
	}
	else {
		mask = UINT8_MAX;
	}

	addattr8(n, MAX_MSG, value_type, value);
	addattr8(n, MAX_MSG, mask_type, mask);

	err = 0;
err:
	if (slash)
		*slash = '/';
	return err;
}

static const char *flower_print_arp_op_to_name(__u8 op)
{
	switch (op) {
	case ARPOP_REQUEST:
		return "request";
	case ARPOP_REPLY:
		return "reply";
	default:
		return NULL;
	}
}

static int flower_arp_op_from_name(const char *name, __u8 *op)
{
	if (!strcmp(name, "request"))
		*op = ARPOP_REQUEST;
	else if (!strcmp(name, "reply"))
		*op = ARPOP_REPLY;
	else
		return -1;

	return 0;
}

static bool flow_arp_op_validate(__u8 op)
{
	return !op || op == ARPOP_REQUEST || op == ARPOP_REPLY;
}

static int flower_parse_arp_op(char *str, __be16 eth_type,
			       int op_type, int mask_type,
			       struct nlmsghdr *n)
{
	if (!flower_eth_type_arp(eth_type))
		return -1;

	return flower_parse_u8(str, op_type, mask_type, flower_arp_op_from_name,
			       flow_arp_op_validate, n);
}

static int flower_icmp_attr_type(__be16 eth_type, __u8 ip_proto,
				 enum flower_icmp_field field)
{
	if (eth_type == htons(ETH_P_IP) && ip_proto == IPPROTO_ICMP)
		return field == FLOWER_ICMP_FIELD_CODE ?
			TCA_FLOWER_KEY_ICMPV4_CODE :
			TCA_FLOWER_KEY_ICMPV4_TYPE;
	else if (eth_type == htons(ETH_P_IPV6) && ip_proto == IPPROTO_ICMPV6)
		return field == FLOWER_ICMP_FIELD_CODE ?
			TCA_FLOWER_KEY_ICMPV6_CODE :
			TCA_FLOWER_KEY_ICMPV6_TYPE;

	return -1;
}

static int flower_icmp_attr_mask_type(__be16 eth_type, __u8 ip_proto,
				      enum flower_icmp_field field)
{
	if (eth_type == htons(ETH_P_IP) && ip_proto == IPPROTO_ICMP)
		return field == FLOWER_ICMP_FIELD_CODE ?
			TCA_FLOWER_KEY_ICMPV4_CODE_MASK :
			TCA_FLOWER_KEY_ICMPV4_TYPE_MASK;
	else if (eth_type == htons(ETH_P_IPV6) && ip_proto == IPPROTO_ICMPV6)
		return field == FLOWER_ICMP_FIELD_CODE ?
			TCA_FLOWER_KEY_ICMPV6_CODE_MASK :
			TCA_FLOWER_KEY_ICMPV6_TYPE_MASK;

	return -1;
}

static int flower_parse_icmp(char *str, __u16 eth_type, __u8 ip_proto,
			     enum flower_icmp_field field, struct nlmsghdr *n)
{
	int value_type, mask_type;

	value_type = flower_icmp_attr_type(eth_type, ip_proto, field);
	mask_type = flower_icmp_attr_mask_type(eth_type, ip_proto, field);
	if (value_type < 0 || mask_type < 0)
		return -1;

	return flower_parse_u8(str, value_type, mask_type, NULL, NULL, n);
}

static int flower_port_attr_type(__u8 ip_proto, enum flower_endpoint endpoint)
{
	//决定采用的port attribute type
	if (ip_proto == IPPROTO_TCP)
		return endpoint == FLOWER_ENDPOINT_SRC ?
			TCA_FLOWER_KEY_TCP_SRC :
			TCA_FLOWER_KEY_TCP_DST;
	else if (ip_proto == IPPROTO_UDP)
		return endpoint == FLOWER_ENDPOINT_SRC ?
			TCA_FLOWER_KEY_UDP_SRC :
			TCA_FLOWER_KEY_UDP_DST;
	else if (ip_proto == IPPROTO_SCTP)
		return endpoint == FLOWER_ENDPOINT_SRC ?
			TCA_FLOWER_KEY_SCTP_SRC :
			TCA_FLOWER_KEY_SCTP_DST;
	else
		return -1;
}

//port range情况下，定义相应的min,max type
static int flower_port_range_attr_type(__u8 ip_proto, enum flower_endpoint type,
				       __be16 *min_port_type,
				       __be16 *max_port_type)
{
	if (ip_proto == IPPROTO_TCP || ip_proto == IPPROTO_UDP ||
	    ip_proto == IPPROTO_SCTP) {
		if (type == FLOWER_ENDPOINT_SRC) {
			*min_port_type = TCA_FLOWER_KEY_PORT_SRC_MIN;
			*max_port_type = TCA_FLOWER_KEY_PORT_SRC_MAX;
		} else {
			*min_port_type = TCA_FLOWER_KEY_PORT_DST_MIN;
			*max_port_type = TCA_FLOWER_KEY_PORT_DST_MAX;
		}
	} else {
		return -1;
	}
	return 0;
}

/* parse range args in format 10-20 */
static int parse_range(char *str, __be16 *min, __be16 *max)
{
	char *sep;

	//解析port范围
	sep = strchr(str, '-');
	if (sep) {
		//解析到两个，执行min,max校验
		*sep = '\0';

		if (get_be16(min, str, 10))
			return -1;

		if (get_be16(max, sep + 1, 10))
			return -1;
	} else {
		//只解析到一个数据
		if (get_be16(min, str, 10))
			return -1;
	}
	return 0;
}

//解析并填充port信息（支持port range格式）
static int flower_parse_port(char *str, __u8 ip_proto,
			     enum flower_endpoint endpoint,
			     struct nlmsghdr *n)
{
	__be16 min = 0;
	__be16 max = 0;
	int ret;

	ret = parse_range(str, &min, &max);
	if (ret)
		return -1;

	if (min && max) {
		__be16 min_port_type, max_port_type;

		//解析到两个，执行min,max校验
		if (max <= min) {
			fprintf(stderr, "max value should be greater than min value\n");
			return -1;
		}
		//获取对应的type
		if (flower_port_range_attr_type(ip_proto, endpoint,
						&min_port_type, &max_port_type))
			return -1;

		//存入port
		addattr16(n, MAX_MSG, min_port_type, min);
		addattr16(n, MAX_MSG, max_port_type, max);
	} else if (min && !max) {
		int type;

		type = flower_port_attr_type(ip_proto, endpoint);
		if (type < 0)
			return -1;
		addattr16(n, MAX_MSG, type, min);
	} else {
		return -1;
	}
	return 0;
}

#define TCP_FLAGS_MAX_MASK 0xfff

static int flower_parse_tcp_flags(char *str, int flags_type, int mask_type,
				  struct nlmsghdr *n)
{
	char *slash;
	int ret, err = -1;
	__u16 flags;

	slash = strchr(str, '/');
	if (slash)
		*slash = '\0';

	ret = get_u16(&flags, str, 16);
	if (ret < 0 || flags & ~TCP_FLAGS_MAX_MASK)
		goto err;

	addattr16(n, MAX_MSG, flags_type, htons(flags));

	if (slash) {
		//支持tcp_flags的掩码形式
		ret = get_u16(&flags, slash + 1, 16);
		if (ret < 0 || flags & ~TCP_FLAGS_MAX_MASK)
			goto err;
	} else {
		flags = TCP_FLAGS_MAX_MASK;
	}
	addattr16(n, MAX_MSG, mask_type, htons(flags));

	err = 0;
err:
	if (slash)
		*slash = '/';
	return err;
}

static int flower_parse_ip_tos_ttl(char *str, int key_type, int mask_type,
				   struct nlmsghdr *n)
{
	char *slash;
	int ret, err = -1;
	__u8 tos_ttl;

	slash = strchr(str, '/');
	if (slash)
		*slash = '\0';

	ret = get_u8(&tos_ttl, str, 10);
	if (ret < 0)
		ret = get_u8(&tos_ttl, str, 16);
	if (ret < 0)
		goto err;

	addattr8(n, MAX_MSG, key_type, tos_ttl);

	if (slash) {
		ret = get_u8(&tos_ttl, slash + 1, 16);
		if (ret < 0)
			goto err;
	} else {
		tos_ttl = 0xff;
	}
	addattr8(n, MAX_MSG, mask_type, tos_ttl);

	err = 0;
err:
	if (slash)
		*slash = '/';
	return err;
}

static int flower_parse_key_id(const char *str, int type, struct nlmsghdr *n)
{
	int ret;
	__be32 key_id;

	ret = get_be32(&key_id, str, 10);
	if (!ret)
		addattr32(n, MAX_MSG, type, key_id);

	return ret;
}

static int flower_parse_enc_port(char *str, int type, struct nlmsghdr *n)
{
	int ret;
	__be16 port;

	ret = get_be16(&port, str, 10);
	if (ret)
		return -1;

	addattr16(n, MAX_MSG, type, port);

	return 0;
}

static int flower_parse_geneve_opts(char *str, struct nlmsghdr *n)
{
	struct rtattr *nest;
	char *token;
	int i, err;

	nest = addattr_nest(n, MAX_MSG, TCA_FLOWER_KEY_ENC_OPTS_GENEVE);

	i = 1;
	token = strsep(&str, ":");
	while (token) {
		switch (i) {
		case TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS:
		{
			__be16 opt_class;

			if (!strlen(token))
				break;
			err = get_be16(&opt_class, token, 16);
			if (err)
				return err;

			addattr16(n, MAX_MSG, i, opt_class);
			break;
		}
		case TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE:
		{
			__u8 opt_type;

			if (!strlen(token))
				break;
			err = get_u8(&opt_type, token, 16);
			if (err)
				return err;

			addattr8(n, MAX_MSG, i, opt_type);
			break;
		}
		case TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA:
		{
			size_t token_len = strlen(token);
			__u8 *opts;

			if (!token_len)
				break;
			opts = malloc(token_len / 2);
			if (!opts)
				return -1;
			if (hex2mem(token, opts, token_len / 2) < 0) {
				free(opts);
				return -1;
			}
			addattr_l(n, MAX_MSG, i, opts, token_len / 2);
			free(opts);

			break;
		}
		default:
			fprintf(stderr, "Unknown \"geneve_opts\" type\n");
			return -1;
		}

		token = strsep(&str, ":");
		i++;
	}
	addattr_nest_end(n, nest);

	return 0;
}

static int flower_parse_enc_opt_part(char *str, struct nlmsghdr *n)
{
	char *token;
	int err;

	token = strsep(&str, ",");
	while (token) {
		err = flower_parse_geneve_opts(token, n);
		if (err)
			return err;

		token = strsep(&str, ",");
	}

	return 0;
}

static int flower_check_enc_opt_key(char *key)
{
	int key_len, col_cnt = 0;

	key_len = strlen(key);
	while ((key = strchr(key, ':'))) {
		if (strlen(key) == key_len)
			return -1;

		key_len = strlen(key) - 1;
		col_cnt++;
		key++;
	}

	if (col_cnt != 2 || !key_len)
		return -1;

	return 0;
}

static int flower_parse_enc_opts(char *str, struct nlmsghdr *n)
{
	char key[XATTR_SIZE_MAX], mask[XATTR_SIZE_MAX];
	int data_len, key_len, mask_len, err;
	char *token, *slash;
	struct rtattr *nest;

	key_len = 0;
	mask_len = 0;
	token = strsep(&str, ",");
	while (token) {
		slash = strchr(token, '/');
		if (slash)
			*slash = '\0';

		if ((key_len + strlen(token) > XATTR_SIZE_MAX) ||
		    flower_check_enc_opt_key(token))
			return -1;

		strcpy(&key[key_len], token);
		key_len += strlen(token) + 1;
		key[key_len - 1] = ',';

		if (!slash) {
			/* Pad out mask when not provided */
			if (mask_len + strlen(token) > XATTR_SIZE_MAX)
				return -1;

			data_len = strlen(rindex(token, ':'));
			sprintf(&mask[mask_len], "ffff:ff:");
			mask_len += 8;
			memset(&mask[mask_len], 'f', data_len - 1);
			mask_len += data_len;
			mask[mask_len - 1] = ',';
			token = strsep(&str, ",");
			continue;
		}

		if (mask_len + strlen(slash + 1) > XATTR_SIZE_MAX)
			return -1;

		strcpy(&mask[mask_len], slash + 1);
		mask_len += strlen(slash + 1) + 1;
		mask[mask_len - 1] = ',';

		*slash = '/';
		token = strsep(&str, ",");
	}
	key[key_len - 1] = '\0';
	mask[mask_len - 1] = '\0';

	nest = addattr_nest(n, MAX_MSG, TCA_FLOWER_KEY_ENC_OPTS);
	err = flower_parse_enc_opt_part(key, n);
	if (err)
		return err;
	addattr_nest_end(n, nest);

	nest = addattr_nest(n, MAX_MSG, TCA_FLOWER_KEY_ENC_OPTS_MASK);
	err = flower_parse_enc_opt_part(mask, n);
	if (err)
		return err;
	addattr_nest_end(n, nest);

	return 0;
}

static int flower_parse_opt(struct filter_util *qu, char *handle,
			    int argc, char **argv, struct nlmsghdr *n/*出参，保存解析的参数*/)
{
	int ret;
	struct tcmsg *t = NLMSG_DATA(n);
	struct rtattr *tail;
	//filter对应的protocol type
	__be16 eth_type = TC_H_MIN(t->tcm_info);
	__be16 vlan_ethtype = 0;
	__be16 cvlan_ethtype = 0;
	__u8 ip_proto = 0xff;
	__u32 flags = 0;/*规则flag*/
	__u32 mtf = 0;
	__u32 mtf_mask = 0;

	if (handle) {
		//如果配置了handle,将handle转换为u32整数
		ret = get_u32(&t->tcm_handle, handle, 0);
		if (ret) {
			fprintf(stderr, "Illegal \"handle\"\n");
			return -1;
		}
	}

	tail = (struct rtattr *) (((void *) n) + NLMSG_ALIGN(n->nlmsg_len));
	//添加options,暂不支持其实际大小及数据
	addattr_l(n, MAX_MSG, TCA_OPTIONS, NULL, 0);

	if (argc == 0) {
		/*at minimal we will match all ethertype packets */
		goto parse_done;
	}

	while (argc > 0) {
		if (matches(*argv, "classid") == 0 ||
		    matches(*argv, "flowid") == 0) {
			unsigned int handle;

			NEXT_ARG();
			ret = get_tc_classid(&handle, *argv);
			if (ret) {
				fprintf(stderr, "Illegal \"classid\"\n");
				return -1;
			}
			addattr_l(n, MAX_MSG, TCA_FLOWER_CLASSID, &handle, 4);
		} else if (matches(*argv, "hw_tc") == 0) {
			unsigned int handle;
			__u32 tc;
			char *end;

			NEXT_ARG();
			tc = strtoul(*argv, &end, 0);
			if (*end) {
				fprintf(stderr, "Illegal TC index\n");
				return -1;
			}
			if (tc >= TC_QOPT_MAX_QUEUE) {
				fprintf(stderr, "TC index exceeds max range\n");
				return -1;
			}
			handle = TC_H_MAKE(TC_H_MAJ(t->tcm_parent),
					   TC_H_MIN(tc + TC_H_MIN_PRIORITY));
			addattr_l(n, MAX_MSG, TCA_FLOWER_CLASSID, &handle,
				  sizeof(handle));
		} else if (matches(*argv, "ip_flags") == 0) {
			NEXT_ARG();
			ret = flower_parse_matching_flags(*argv,
							  FLOWER_IP_FLAGS,
							  &mtf,
							  &mtf_mask);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"ip_flags\"\n");
				return -1;
			}
		} else if (matches(*argv, "verbose") == 0) {
			flags |= TCA_CLS_FLAGS_VERBOSE;
		} else if (matches(*argv, "skip_hw") == 0) {
			//指明skip hardware
			flags |= TCA_CLS_FLAGS_SKIP_HW;
		} else if (matches(*argv, "skip_sw") == 0) {
			//指明skip software
			flags |= TCA_CLS_FLAGS_SKIP_SW;
		} else if (matches(*argv, "indev") == 0) {
			//指明indev,采用哪个入接口设备
			NEXT_ARG();
			if (check_ifname(*argv))
				invarg("\"indev\" not a valid ifname", *argv);
			addattrstrz(n, MAX_MSG, TCA_FLOWER_INDEV, *argv);
		} else if (matches(*argv, "vlan_id") == 0) {
			__u16 vid;

			NEXT_ARG();
			if (!eth_type_vlan(eth_type)) {
				fprintf(stderr, "Can't set \"vlan_id\" if ethertype isn't 802.1Q or 802.1AD\n");
				return -1;
			}
			ret = get_u16(&vid, *argv, 10);
			if (ret < 0 || vid & ~0xfff) {
				fprintf(stderr, "Illegal \"vlan_id\"\n");
				return -1;
			}
			addattr16(n, MAX_MSG, TCA_FLOWER_KEY_VLAN_ID, vid);
		} else if (matches(*argv, "vlan_prio") == 0) {
			__u8 vlan_prio;

			NEXT_ARG();
			if (!eth_type_vlan(eth_type)) {
				fprintf(stderr, "Can't set \"vlan_prio\" if ethertype isn't 802.1Q or 802.1AD\n");
				return -1;
			}
			ret = get_u8(&vlan_prio, *argv, 10);
			if (ret < 0 || vlan_prio & ~0x7) {
				fprintf(stderr, "Illegal \"vlan_prio\"\n");
				return -1;
			}
			addattr8(n, MAX_MSG,
				 TCA_FLOWER_KEY_VLAN_PRIO, vlan_prio);
		} else if (matches(*argv, "vlan_ethtype") == 0) {
			NEXT_ARG();
			ret = flower_parse_vlan_eth_type(*argv, eth_type,
						 TCA_FLOWER_KEY_VLAN_ETH_TYPE,
						 &vlan_ethtype, n);
			if (ret < 0)
				return -1;
		} else if (matches(*argv, "cvlan_id") == 0) {
			__u16 vid;

			NEXT_ARG();
			if (!eth_type_vlan(vlan_ethtype)) {
				fprintf(stderr, "Can't set \"cvlan_id\" if inner vlan ethertype isn't 802.1Q or 802.1AD\n");
				return -1;
			}
			ret = get_u16(&vid, *argv, 10);
			if (ret < 0 || vid & ~0xfff) {
				fprintf(stderr, "Illegal \"cvlan_id\"\n");
				return -1;
			}
			addattr16(n, MAX_MSG, TCA_FLOWER_KEY_CVLAN_ID, vid);
		} else if (matches(*argv, "cvlan_prio") == 0) {
			__u8 cvlan_prio;

			NEXT_ARG();
			if (!eth_type_vlan(vlan_ethtype)) {
				fprintf(stderr, "Can't set \"cvlan_prio\" if inner vlan ethertype isn't 802.1Q or 802.1AD\n");
				return -1;
			}
			ret = get_u8(&cvlan_prio, *argv, 10);
			if (ret < 0 || cvlan_prio & ~0x7) {
				fprintf(stderr, "Illegal \"cvlan_prio\"\n");
				return -1;
			}
			addattr8(n, MAX_MSG,
				 TCA_FLOWER_KEY_CVLAN_PRIO, cvlan_prio);
		} else if (matches(*argv, "cvlan_ethtype") == 0) {
			NEXT_ARG();
			ret = flower_parse_vlan_eth_type(*argv, vlan_ethtype,
						 TCA_FLOWER_KEY_CVLAN_ETH_TYPE,
						 &cvlan_ethtype, n);
			if (ret < 0)
				return -1;
		} else if (matches(*argv, "mpls_label") == 0) {
			__u32 label;

			NEXT_ARG();
			if (eth_type != htons(ETH_P_MPLS_UC) &&
			    eth_type != htons(ETH_P_MPLS_MC)) {
				fprintf(stderr,
					"Can't set \"mpls_label\" if ethertype isn't MPLS\n");
				return -1;
			}
			ret = get_u32(&label, *argv, 10);
			if (ret < 0 || label & ~(MPLS_LS_LABEL_MASK >> MPLS_LS_LABEL_SHIFT)) {
				fprintf(stderr, "Illegal \"mpls_label\"\n");
				return -1;
			}
			addattr32(n, MAX_MSG, TCA_FLOWER_KEY_MPLS_LABEL, label);
		} else if (matches(*argv, "mpls_tc") == 0) {
			__u8 tc;

			NEXT_ARG();
			if (eth_type != htons(ETH_P_MPLS_UC) &&
			    eth_type != htons(ETH_P_MPLS_MC)) {
				fprintf(stderr,
					"Can't set \"mpls_tc\" if ethertype isn't MPLS\n");
				return -1;
			}
			ret = get_u8(&tc, *argv, 10);
			if (ret < 0 || tc & ~(MPLS_LS_TC_MASK >> MPLS_LS_TC_SHIFT)) {
				fprintf(stderr, "Illegal \"mpls_tc\"\n");
				return -1;
			}
			addattr8(n, MAX_MSG, TCA_FLOWER_KEY_MPLS_TC, tc);
		} else if (matches(*argv, "mpls_bos") == 0) {
			__u8 bos;

			NEXT_ARG();
			if (eth_type != htons(ETH_P_MPLS_UC) &&
			    eth_type != htons(ETH_P_MPLS_MC)) {
				fprintf(stderr,
					"Can't set \"mpls_bos\" if ethertype isn't MPLS\n");
				return -1;
			}
			ret = get_u8(&bos, *argv, 10);
			if (ret < 0 || bos & ~(MPLS_LS_S_MASK >> MPLS_LS_S_SHIFT)) {
				fprintf(stderr, "Illegal \"mpls_bos\"\n");
				return -1;
			}
			addattr8(n, MAX_MSG, TCA_FLOWER_KEY_MPLS_BOS, bos);
		} else if (matches(*argv, "mpls_ttl") == 0) {
			__u8 ttl;

			NEXT_ARG();
			if (eth_type != htons(ETH_P_MPLS_UC) &&
			    eth_type != htons(ETH_P_MPLS_MC)) {
				fprintf(stderr,
					"Can't set \"mpls_ttl\" if ethertype isn't MPLS\n");
				return -1;
			}
			ret = get_u8(&ttl, *argv, 10);
			if (ret < 0 || ttl & ~(MPLS_LS_TTL_MASK >> MPLS_LS_TTL_SHIFT)) {
				fprintf(stderr, "Illegal \"mpls_ttl\"\n");
				return -1;
			}
			addattr8(n, MAX_MSG, TCA_FLOWER_KEY_MPLS_TTL, ttl);
		} else if (matches(*argv, "dst_mac") == 0) {
			NEXT_ARG();
			ret = flower_parse_eth_addr(*argv,
						    TCA_FLOWER_KEY_ETH_DST,
						    TCA_FLOWER_KEY_ETH_DST_MASK,
						    n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"dst_mac\"\n");
				return -1;
			}
		} else if (matches(*argv, "src_mac") == 0) {
			NEXT_ARG();
			//解析并存入srcmac
			ret = flower_parse_eth_addr(*argv,
						    TCA_FLOWER_KEY_ETH_SRC,
						    TCA_FLOWER_KEY_ETH_SRC_MASK,
						    n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"src_mac\"\n");
				return -1;
			}
		} else if (matches(*argv, "ip_proto") == 0) {
			//定义ip的上层协议类型，例如tcp,udp等
			NEXT_ARG();
			ret = flower_parse_ip_proto(*argv, cvlan_ethtype ?
						    cvlan_ethtype : vlan_ethtype ?
						    vlan_ethtype : eth_type,
						    TCA_FLOWER_KEY_IP_PROTO,
						    &ip_proto, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"ip_proto\"\n");
				return -1;
			}
		} else if (matches(*argv, "ip_tos") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_tos_ttl(*argv,
						      TCA_FLOWER_KEY_IP_TOS,
						      TCA_FLOWER_KEY_IP_TOS_MASK,
						      n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"ip_tos\"\n");
				return -1;
			}
		} else if (matches(*argv, "ip_ttl") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_tos_ttl(*argv,
						      TCA_FLOWER_KEY_IP_TTL,
						      TCA_FLOWER_KEY_IP_TTL_MASK,
						      n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"ip_ttl\"\n");
				return -1;
			}
		} else if (matches(*argv, "dst_ip") == 0) {
			NEXT_ARG();
			//解析目的ip及其mask
			ret = flower_parse_ip_addr(*argv, cvlan_ethtype ?
						   cvlan_ethtype : vlan_ethtype ?
						   vlan_ethtype : eth_type,
						   TCA_FLOWER_KEY_IPV4_DST,
						   TCA_FLOWER_KEY_IPV4_DST_MASK,
						   TCA_FLOWER_KEY_IPV6_DST,
						   TCA_FLOWER_KEY_IPV6_DST_MASK,
						   n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"dst_ip\"\n");
				return -1;
			}
		} else if (matches(*argv, "src_ip") == 0) {
			//解析源ip及mask
			NEXT_ARG();
			ret = flower_parse_ip_addr(*argv, cvlan_ethtype ?
						   cvlan_ethtype : vlan_ethtype ?
						   vlan_ethtype : eth_type,
						   TCA_FLOWER_KEY_IPV4_SRC,
						   TCA_FLOWER_KEY_IPV4_SRC_MASK,
						   TCA_FLOWER_KEY_IPV6_SRC,
						   TCA_FLOWER_KEY_IPV6_SRC_MASK,
						   n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"src_ip\"\n");
				return -1;
			}
		} else if (matches(*argv, "dst_port") == 0) {
			NEXT_ARG();
			ret = flower_parse_port(*argv, ip_proto,
						FLOWER_ENDPOINT_DST, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"dst_port\"\n");
				return -1;
			}
		} else if (matches(*argv, "src_port") == 0) {
			NEXT_ARG();
			//解析填充src_port,支持range形式
			ret = flower_parse_port(*argv, ip_proto/*上层协义*/,
						FLOWER_ENDPOINT_SRC, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"src_port\"\n");
				return -1;
			}
		} else if (matches(*argv, "tcp_flags") == 0) {
			NEXT_ARG();
			//解析并填充tcp_flags
			ret = flower_parse_tcp_flags(*argv,
						     TCA_FLOWER_KEY_TCP_FLAGS,
						     TCA_FLOWER_KEY_TCP_FLAGS_MASK,
						     n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"tcp_flags\"\n");
				return -1;
			}
		} else if (matches(*argv, "type") == 0) {
			NEXT_ARG();
			ret = flower_parse_icmp(*argv, eth_type, ip_proto,
						FLOWER_ICMP_FIELD_TYPE, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"icmp type\"\n");
				return -1;
			}
		} else if (matches(*argv, "code") == 0) {
			NEXT_ARG();
			ret = flower_parse_icmp(*argv, eth_type, ip_proto,
						FLOWER_ICMP_FIELD_CODE, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"icmp code\"\n");
				return -1;
			}
		} else if (matches(*argv, "arp_tip") == 0) {
			NEXT_ARG();
			ret = flower_parse_arp_ip_addr(*argv, vlan_ethtype ?
						       vlan_ethtype : eth_type,
						       TCA_FLOWER_KEY_ARP_TIP,
						       TCA_FLOWER_KEY_ARP_TIP_MASK,
						       n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"arp_tip\"\n");
				return -1;
			}
		} else if (matches(*argv, "arp_sip") == 0) {
			NEXT_ARG();
			ret = flower_parse_arp_ip_addr(*argv, vlan_ethtype ?
						       vlan_ethtype : eth_type,
						       TCA_FLOWER_KEY_ARP_SIP,
						       TCA_FLOWER_KEY_ARP_SIP_MASK,
						       n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"arp_sip\"\n");
				return -1;
			}
		} else if (matches(*argv, "arp_op") == 0) {
			NEXT_ARG();
			ret = flower_parse_arp_op(*argv, vlan_ethtype ?
						  vlan_ethtype : eth_type,
						  TCA_FLOWER_KEY_ARP_OP,
						  TCA_FLOWER_KEY_ARP_OP_MASK,
						  n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"arp_op\"\n");
				return -1;
			}
		} else if (matches(*argv, "arp_tha") == 0) {
			NEXT_ARG();
			ret = flower_parse_eth_addr(*argv,
						    TCA_FLOWER_KEY_ARP_THA,
						    TCA_FLOWER_KEY_ARP_THA_MASK,
						    n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"arp_tha\"\n");
				return -1;
			}
		} else if (matches(*argv, "arp_sha") == 0) {
			NEXT_ARG();
			ret = flower_parse_eth_addr(*argv,
						    TCA_FLOWER_KEY_ARP_SHA,
						    TCA_FLOWER_KEY_ARP_SHA_MASK,
						    n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"arp_sha\"\n");
				return -1;
			}
		} else if (matches(*argv, "enc_dst_ip") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_addr(*argv, 0,
						   TCA_FLOWER_KEY_ENC_IPV4_DST,
						   TCA_FLOWER_KEY_ENC_IPV4_DST_MASK,
						   TCA_FLOWER_KEY_ENC_IPV6_DST,
						   TCA_FLOWER_KEY_ENC_IPV6_DST_MASK,
						   n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"enc_dst_ip\"\n");
				return -1;
			}
		} else if (matches(*argv, "enc_src_ip") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_addr(*argv, 0,
						   TCA_FLOWER_KEY_ENC_IPV4_SRC,
						   TCA_FLOWER_KEY_ENC_IPV4_SRC_MASK,
						   TCA_FLOWER_KEY_ENC_IPV6_SRC,
						   TCA_FLOWER_KEY_ENC_IPV6_SRC_MASK,
						   n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"enc_src_ip\"\n");
				return -1;
			}
		} else if (matches(*argv, "enc_key_id") == 0) {
			NEXT_ARG();
			ret = flower_parse_key_id(*argv,
						  TCA_FLOWER_KEY_ENC_KEY_ID, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"enc_key_id\"\n");
				return -1;
			}
		} else if (matches(*argv, "enc_dst_port") == 0) {
			NEXT_ARG();
			ret = flower_parse_enc_port(*argv,
						    TCA_FLOWER_KEY_ENC_UDP_DST_PORT, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"enc_dst_port\"\n");
				return -1;
			}
		} else if (matches(*argv, "enc_tos") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_tos_ttl(*argv,
						      TCA_FLOWER_KEY_ENC_IP_TOS,
						      TCA_FLOWER_KEY_ENC_IP_TOS_MASK,
						      n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"enc_tos\"\n");
				return -1;
			}
		} else if (matches(*argv, "enc_ttl") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_tos_ttl(*argv,
						      TCA_FLOWER_KEY_ENC_IP_TTL,
						      TCA_FLOWER_KEY_ENC_IP_TTL_MASK,
						      n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"enc_ttl\"\n");
				return -1;
			}
		} else if (matches(*argv, "geneve_opts") == 0) {
			NEXT_ARG();
			ret = flower_parse_enc_opts(*argv, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"geneve_opts\"\n");
				return -1;
			}
		} else if (matches(*argv, "action") == 0) {
			NEXT_ARG();
			//执行action参数解析
			ret = parse_action(&argc, &argv, TCA_FLOWER_ACT, n);
			if (ret) {
				fprintf(stderr, "Illegal \"action\"\n");
				return -1;
			}
			continue;
		} else if (strcmp(*argv, "help") == 0) {
			explain();
			return -1;
		} else {
			fprintf(stderr, "What is \"%s\"?\n", *argv);
			explain();
			return -1;
		}
		argc--; argv++;
	}

parse_done:
	ret = addattr32(n, MAX_MSG, TCA_FLOWER_FLAGS, flags);
	if (ret)
		return ret;

	if (mtf_mask) {
		ret = addattr32(n, MAX_MSG, TCA_FLOWER_KEY_FLAGS, htonl(mtf));
		if (ret)
			return ret;

		ret = addattr32(n, MAX_MSG, TCA_FLOWER_KEY_FLAGS_MASK, htonl(mtf_mask));
		if (ret)
			return ret;
	}

	if (eth_type != htons(ETH_P_ALL)) {
		ret = addattr16(n, MAX_MSG, TCA_FLOWER_KEY_ETH_TYPE, eth_type);
		if (ret)
			return ret;
	}

	tail->rta_len = (((void *)n)+n->nlmsg_len) - (void *)tail;

	return 0;
}

static int __mask_bits(char *addr, size_t len)
{
	int bits = 0;
	bool hole = false;
	int i;
	int j;

	for (i = 0; i < len; i++, addr++) {
		for (j = 7; j >= 0; j--) {
			if (((*addr) >> j) & 0x1) {
				if (hole)
					return -1;
				bits++;
			} else if (bits) {
				hole = true;
			} else{
				return -1;
			}
		}
	}
	return bits;
}

static void flower_print_eth_addr(char *name, struct rtattr *addr_attr,
				  struct rtattr *mask_attr)
{
	SPRINT_BUF(namefrm);
	SPRINT_BUF(out);
	SPRINT_BUF(b1);
	size_t done;
	int bits;

	if (!addr_attr || RTA_PAYLOAD(addr_attr) != ETH_ALEN)
		return;
	done = sprintf(out, "%s",
		       ll_addr_n2a(RTA_DATA(addr_attr), ETH_ALEN,
				   0, b1, sizeof(b1)));
	if (mask_attr && RTA_PAYLOAD(mask_attr) == ETH_ALEN) {
		bits = __mask_bits(RTA_DATA(mask_attr), ETH_ALEN);
		if (bits < 0)
			sprintf(out + done, "/%s",
				ll_addr_n2a(RTA_DATA(mask_attr), ETH_ALEN,
					    0, b1, sizeof(b1)));
		else if (bits < ETH_ALEN * 8)
			sprintf(out + done, "/%d", bits);
	}

	sprintf(namefrm, "\n  %s %%s", name);
	print_string(PRINT_ANY, name, namefrm, out);
}

//显示以太类型
static void flower_print_eth_type(__be16 *p_eth_type,
				  struct rtattr *eth_type_attr)
{
	SPRINT_BUF(out);
	__be16 eth_type;

	if (!eth_type_attr)
		return;

	eth_type = rta_getattr_u16(eth_type_attr);
	if (eth_type == htons(ETH_P_IP))
		sprintf(out, "ipv4");
	else if (eth_type == htons(ETH_P_IPV6))
		sprintf(out, "ipv6");
	else if (eth_type == htons(ETH_P_ARP))
		sprintf(out, "arp");
	else if (eth_type == htons(ETH_P_RARP))
		sprintf(out, "rarp");
	else
		sprintf(out, "%04x", ntohs(eth_type));

	print_string(PRINT_ANY, "eth_type", "\n  eth_type %s", out);
	*p_eth_type = eth_type;
}

//显示ip层协议号
static void flower_print_ip_proto(__u8 *p_ip_proto,
				  struct rtattr *ip_proto_attr)
{
	SPRINT_BUF(out);
	__u8 ip_proto;

	if (!ip_proto_attr)
		return;

	ip_proto = rta_getattr_u8(ip_proto_attr);
	if (ip_proto == IPPROTO_TCP)
		sprintf(out, "tcp");
	else if (ip_proto == IPPROTO_UDP)
		sprintf(out, "udp");
	else if (ip_proto == IPPROTO_SCTP)
		sprintf(out, "sctp");
	else if (ip_proto == IPPROTO_ICMP)
		sprintf(out, "icmp");
	else if (ip_proto == IPPROTO_ICMPV6)
		sprintf(out, "icmpv6");
	else
		sprintf(out, "%02x", ip_proto);

	print_string(PRINT_ANY, "ip_proto", "\n  ip_proto %s", out);
	*p_ip_proto = ip_proto;
}

static void flower_print_ip_attr(const char *name, struct rtattr *key_attr,
				 struct rtattr *mask_attr)
{
	SPRINT_BUF(namefrm);
	SPRINT_BUF(out);
	size_t done;

	if (!key_attr)
		return;

	done = sprintf(out, "0x%x", rta_getattr_u8(key_attr));
	if (mask_attr)
		sprintf(out + done, "/%x", rta_getattr_u8(mask_attr));

	print_string(PRINT_FP, NULL, "%s  ", _SL_);
	sprintf(namefrm, "%s %%s", name);
	print_string(PRINT_ANY, name, namefrm, out);
}

static void flower_print_matching_flags(char *name,
					enum flower_matching_flags type,
					struct rtattr *attr,
					struct rtattr *mask_attr)
{
	int i;
	int count = 0;
	__u32 mtf;
	__u32 mtf_mask;

	if (!mask_attr || RTA_PAYLOAD(mask_attr) != 4)
		return;

	mtf = ntohl(rta_getattr_u32(attr));
	mtf_mask = ntohl(rta_getattr_u32(mask_attr));

	for (i = 0; i < ARRAY_SIZE(flags_str); i++) {
		if (type != flags_str[i].type)
			continue;
		if (mtf_mask & flags_str[i].flag) {
			if (++count == 1) {
				print_string(PRINT_FP, NULL, "\n  %s ", name);
				open_json_object(name);
			} else {
				print_string(PRINT_FP, NULL, "/", NULL);
			}

			print_bool(PRINT_JSON, flags_str[i].string, NULL,
				   mtf & flags_str[i].flag);
			if (mtf & flags_str[i].flag)
				print_string(PRINT_FP, NULL, "%s",
					     flags_str[i].string);
			else
				print_string(PRINT_FP, NULL, "no%s",
					     flags_str[i].string);
		}
	}
	if (count)
		close_json_object();
}

static void flower_print_ip_addr(char *name, __be16 eth_type,
				 struct rtattr *addr4_attr,
				 struct rtattr *mask4_attr,
				 struct rtattr *addr6_attr,
				 struct rtattr *mask6_attr)
{
	struct rtattr *addr_attr;
	struct rtattr *mask_attr;
	SPRINT_BUF(namefrm);
	SPRINT_BUF(out);
	size_t done;
	int family;
	size_t len;
	int bits;

	if (eth_type == htons(ETH_P_IP)) {
		family = AF_INET;
		addr_attr = addr4_attr;
		mask_attr = mask4_attr;
		len = 4;
	} else if (eth_type == htons(ETH_P_IPV6)) {
		family = AF_INET6;
		addr_attr = addr6_attr;
		mask_attr = mask6_attr;
		len = 16;
	} else {
		return;
	}
	if (!addr_attr || RTA_PAYLOAD(addr_attr) != len)
		return;
	if (!mask_attr || RTA_PAYLOAD(mask_attr) != len)
		return;
	done = sprintf(out, "%s", rt_addr_n2a_rta(family, addr_attr));
	bits = __mask_bits(RTA_DATA(mask_attr), len);
	if (bits < 0)
		sprintf(out + done, "/%s", rt_addr_n2a_rta(family, mask_attr));
	else if (bits < len * 8)
		sprintf(out + done, "/%d", bits);

	sprintf(namefrm, "\n  %s %%s", name);
	print_string(PRINT_ANY, name, namefrm, out);
}
static void flower_print_ip4_addr(char *name, struct rtattr *addr_attr,
				  struct rtattr *mask_attr)
{
	return flower_print_ip_addr(name, htons(ETH_P_IP),
				    addr_attr, mask_attr, 0, 0);
}

static void flower_print_port(char *name, struct rtattr *attr)
{
	SPRINT_BUF(namefrm);

	if (!attr)
		return;

	sprintf(namefrm,"\n  %s %%u", name);
	print_hu(PRINT_ANY, name, namefrm, rta_getattr_be16(attr));
}

static void flower_print_port_range(char *name, struct rtattr *min_attr,
				    struct rtattr *max_attr)
{
	if (!min_attr || !max_attr)
		return;

	if (is_json_context()) {
		open_json_object(name);
		print_hu(PRINT_JSON, "start", NULL, rta_getattr_be16(min_attr));
		print_hu(PRINT_JSON, "end", NULL, rta_getattr_be16(max_attr));
		close_json_object();
	} else {
		SPRINT_BUF(namefrm);
		SPRINT_BUF(out);
		size_t done;

		done = sprintf(out, "%u", rta_getattr_be16(min_attr));
		sprintf(out + done, "-%u", rta_getattr_be16(max_attr));
		sprintf(namefrm, "\n  %s %%s", name);
		print_string(PRINT_ANY, name, namefrm, out);
	}
}

static void flower_print_tcp_flags(const char *name, struct rtattr *flags_attr,
				   struct rtattr *mask_attr)
{
	SPRINT_BUF(namefrm);
	SPRINT_BUF(out);
	size_t done;

	if (!flags_attr)
		return;

	done = sprintf(out, "0x%x", rta_getattr_be16(flags_attr));
	if (mask_attr)
		sprintf(out + done, "/%x", rta_getattr_be16(mask_attr));

	print_string(PRINT_FP, NULL, "%s  ", _SL_);
	sprintf(namefrm, "%s %%s", name);
	print_string(PRINT_ANY, name, namefrm, out);
}


static void flower_print_key_id(const char *name, struct rtattr *attr)
{
	SPRINT_BUF(namefrm);

	if (!attr)
		return;

	sprintf(namefrm,"\n  %s %%u", name);
	print_uint(PRINT_ANY, name, namefrm, rta_getattr_be32(attr));
}

static void flower_print_geneve_opts(const char *name, struct rtattr *attr,
				     char *strbuf)
{
	struct rtattr *tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_MAX + 1];
	int ii, data_len, offset = 0, slen = 0;
	struct rtattr *i = RTA_DATA(attr);
	int rem = RTA_PAYLOAD(attr);
	__u8 type, data_r[rem];
	char data[rem * 2 + 1];
	__u16 class;

	open_json_array(PRINT_JSON, name);
	while (rem) {
		parse_rtattr(tb, TCA_FLOWER_KEY_ENC_OPT_GENEVE_MAX, i, rem);
		class = rta_getattr_be16(tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS]);
		type = rta_getattr_u8(tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE]);
		data_len = RTA_PAYLOAD(tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA]);
		hexstring_n2a(RTA_DATA(tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA]),
			      data_len, data, sizeof(data));
		hex2mem(data, data_r, data_len);
		offset += data_len + 20;
		rem -= data_len + 20;
		i = RTA_DATA(attr) + offset;

		open_json_object(NULL);
		print_uint(PRINT_JSON, "class", NULL, class);
		print_uint(PRINT_JSON, "type", NULL, type);
		open_json_array(PRINT_JSON, "data");
		for (ii = 0; ii < data_len; ii++)
			print_uint(PRINT_JSON, NULL, NULL, data_r[ii]);
		close_json_array(PRINT_JSON, "data");
		close_json_object();

		slen += sprintf(strbuf + slen, "%04x:%02x:%s",
				class, type, data);
		if (rem)
			slen += sprintf(strbuf + slen, ",");
	}
	close_json_array(PRINT_JSON, name);
}

static void flower_print_geneve_parts(const char *name, struct rtattr *attr,
				      char *key, char *mask)
{
	char *namefrm = "\n  geneve_opt %s";
	char *key_token, *mask_token, *out;
	int len;

	out = malloc(RTA_PAYLOAD(attr) * 4 + 3);
	if (!out)
		return;

	len = 0;
	key_token = strsep(&key, ",");
	mask_token = strsep(&mask, ",");
	while (key_token) {
		len += sprintf(&out[len], "%s/%s,", key_token, mask_token);
		mask_token = strsep(&mask, ",");
		key_token = strsep(&key, ",");
	}

	out[len - 1] = '\0';
	print_string(PRINT_FP, name, namefrm, out);
	free(out);
}

static void flower_print_enc_opts(const char *name, struct rtattr *attr,
				  struct rtattr *mask_attr)
{
	struct rtattr *key_tb[TCA_FLOWER_KEY_ENC_OPTS_MAX + 1];
	struct rtattr *msk_tb[TCA_FLOWER_KEY_ENC_OPTS_MAX + 1];
	char *key, *msk;

	if (!attr)
		return;

	key = malloc(RTA_PAYLOAD(attr) * 2 + 1);
	if (!key)
		return;

	msk = malloc(RTA_PAYLOAD(attr) * 2 + 1);
	if (!msk)
		goto err_key_free;

	parse_rtattr_nested(key_tb, TCA_FLOWER_KEY_ENC_OPTS_MAX, attr);
	flower_print_geneve_opts("geneve_opt_key",
				 key_tb[TCA_FLOWER_KEY_ENC_OPTS_GENEVE], key);

	parse_rtattr_nested(msk_tb, TCA_FLOWER_KEY_ENC_OPTS_MAX, mask_attr);
	flower_print_geneve_opts("geneve_opt_mask",
				 msk_tb[TCA_FLOWER_KEY_ENC_OPTS_GENEVE], msk);

	flower_print_geneve_parts(name, attr, key, msk);

	free(msk);
err_key_free:
	free(key);
}

static void flower_print_masked_u8(const char *name, struct rtattr *attr,
				   struct rtattr *mask_attr,
				   const char *(*value_to_str)(__u8 value))
{
	const char *value_str = NULL;
	__u8 value, mask;
	SPRINT_BUF(namefrm);
	SPRINT_BUF(out);
	size_t done;

	if (!attr)
		return;

	value = rta_getattr_u8(attr);
	mask = mask_attr ? rta_getattr_u8(mask_attr) : UINT8_MAX;
	if (mask == UINT8_MAX && value_to_str)
		value_str = value_to_str(value);

	if (value_str)
		done = sprintf(out, "%s", value_str);
	else
		done = sprintf(out, "%d", value);

	if (mask != UINT8_MAX)
		sprintf(out + done, "/%d", mask);

	sprintf(namefrm,"\n  %s %%s", name);
	print_string(PRINT_ANY, name, namefrm, out);
}

static void flower_print_u8(const char *name, struct rtattr *attr)
{
	flower_print_masked_u8(name, attr, NULL, NULL);
}

static void flower_print_u32(const char *name, struct rtattr *attr)
{
	SPRINT_BUF(namefrm);

	if (!attr)
		return;

	sprintf(namefrm,"\n  %s %%u", name);
	print_uint(PRINT_ANY, name, namefrm, rta_getattr_u32(attr));
}

static void flower_print_arp_op(const char *name,
				struct rtattr *op_attr,
				struct rtattr *mask_attr)
{
	flower_print_masked_u8(name, op_attr, mask_attr,
			       flower_print_arp_op_to_name);
}

//dump flower的配置
static int flower_print_opt(struct filter_util *qu, FILE *f,
			    struct rtattr *opt, __u32 handle)
{
	struct rtattr *tb[TCA_FLOWER_MAX + 1];
	__be16 min_port_type, max_port_type;
	int nl_type, nl_mask_type;
	__be16 eth_type = 0;
	__u8 ip_proto = 0xff;

	if (!opt)
		return 0;

	//解析opt到tbl中，然后分别显示tbl
	parse_rtattr_nested(tb, TCA_FLOWER_MAX, opt);

	if (handle)
		print_uint(PRINT_ANY, "handle", "handle 0x%x ", handle);

	if (tb[TCA_FLOWER_CLASSID]) {
		__u32 h = rta_getattr_u32(tb[TCA_FLOWER_CLASSID]);

		if (TC_H_MIN(h) < TC_H_MIN_PRIORITY ||
		    TC_H_MIN(h) > (TC_H_MIN_PRIORITY + TC_QOPT_MAX_QUEUE - 1)) {
			SPRINT_BUF(b1);
			print_string(PRINT_ANY, "classid", "classid %s ",
				     sprint_tc_classid(h, b1));
		} else {
			print_uint(PRINT_ANY, "hw_tc", "hw_tc %u ",
				   TC_H_MIN(h) - TC_H_MIN_PRIORITY);
		}
	}

	if (tb[TCA_FLOWER_INDEV]) {
		struct rtattr *attr = tb[TCA_FLOWER_INDEV];

		print_string(PRINT_ANY, "indev", "\n  indev %s",
			     rta_getattr_str(attr));
	}

	open_json_object("keys");

	if (tb[TCA_FLOWER_KEY_VLAN_ID]) {
		struct rtattr *attr = tb[TCA_FLOWER_KEY_VLAN_ID];

		print_uint(PRINT_ANY, "vlan_id", "\n  vlan_id %u",
			   rta_getattr_u16(attr));
	}

	if (tb[TCA_FLOWER_KEY_VLAN_PRIO]) {
		struct rtattr *attr = tb[TCA_FLOWER_KEY_VLAN_PRIO];

		print_uint(PRINT_ANY, "vlan_prio", "\n  vlan_prio %d",
			   rta_getattr_u8(attr));
	}

	if (tb[TCA_FLOWER_KEY_VLAN_ETH_TYPE]) {
		SPRINT_BUF(buf);
		struct rtattr *attr = tb[TCA_FLOWER_KEY_VLAN_ETH_TYPE];

		print_string(PRINT_ANY, "vlan_ethtype", "\n  vlan_ethtype %s",
			     ll_proto_n2a(rta_getattr_u16(attr),
			     buf, sizeof(buf)));
	}

	if (tb[TCA_FLOWER_KEY_CVLAN_ID]) {
		struct rtattr *attr = tb[TCA_FLOWER_KEY_CVLAN_ID];

		print_uint(PRINT_ANY, "cvlan_id", "\n  cvlan_id %u",
			   rta_getattr_u16(attr));
	}

	if (tb[TCA_FLOWER_KEY_CVLAN_PRIO]) {
		struct rtattr *attr = tb[TCA_FLOWER_KEY_CVLAN_PRIO];

		print_uint(PRINT_ANY, "cvlan_prio", "\n  cvlan_prio %d",
			   rta_getattr_u8(attr));
	}

	if (tb[TCA_FLOWER_KEY_CVLAN_ETH_TYPE]) {
		SPRINT_BUF(buf);
		struct rtattr *attr = tb[TCA_FLOWER_KEY_CVLAN_ETH_TYPE];

		print_string(PRINT_ANY, "cvlan_ethtype", "\n  cvlan_ethtype %s",
			     ll_proto_n2a(rta_getattr_u16(attr),
			     buf, sizeof(buf)));
	}

	//显示目的mac
	flower_print_eth_addr("dst_mac", tb[TCA_FLOWER_KEY_ETH_DST],
			      tb[TCA_FLOWER_KEY_ETH_DST_MASK]);
	//显示源mac
	flower_print_eth_addr("src_mac", tb[TCA_FLOWER_KEY_ETH_SRC],
			      tb[TCA_FLOWER_KEY_ETH_SRC_MASK]);

	flower_print_eth_type(&eth_type, tb[TCA_FLOWER_KEY_ETH_TYPE]);
	flower_print_ip_proto(&ip_proto, tb[TCA_FLOWER_KEY_IP_PROTO]);

	flower_print_ip_attr("ip_tos", tb[TCA_FLOWER_KEY_IP_TOS],
			    tb[TCA_FLOWER_KEY_IP_TOS_MASK]);
	flower_print_ip_attr("ip_ttl", tb[TCA_FLOWER_KEY_IP_TTL],
			    tb[TCA_FLOWER_KEY_IP_TTL_MASK]);

	flower_print_u32("mpls_label", tb[TCA_FLOWER_KEY_MPLS_LABEL]);
	flower_print_u8("mpls_tc", tb[TCA_FLOWER_KEY_MPLS_TC]);
	flower_print_u8("mpls_bos", tb[TCA_FLOWER_KEY_MPLS_BOS]);
	flower_print_u8("mpls_ttl", tb[TCA_FLOWER_KEY_MPLS_TTL]);

	//显示目的ip
	flower_print_ip_addr("dst_ip", eth_type,
			     tb[TCA_FLOWER_KEY_IPV4_DST],
			     tb[TCA_FLOWER_KEY_IPV4_DST_MASK],
			     tb[TCA_FLOWER_KEY_IPV6_DST],
			     tb[TCA_FLOWER_KEY_IPV6_DST_MASK]);

	//显示源ip
	flower_print_ip_addr("src_ip", eth_type,
			     tb[TCA_FLOWER_KEY_IPV4_SRC],
			     tb[TCA_FLOWER_KEY_IPV4_SRC_MASK],
			     tb[TCA_FLOWER_KEY_IPV6_SRC],
			     tb[TCA_FLOWER_KEY_IPV6_SRC_MASK]);

	nl_type = flower_port_attr_type(ip_proto, FLOWER_ENDPOINT_DST);
	if (nl_type >= 0)
		flower_print_port("dst_port", tb[nl_type]);
	nl_type = flower_port_attr_type(ip_proto, FLOWER_ENDPOINT_SRC);
	if (nl_type >= 0)
		flower_print_port("src_port", tb[nl_type]);

	if (!flower_port_range_attr_type(ip_proto, FLOWER_ENDPOINT_DST,
					 &min_port_type, &max_port_type))
		flower_print_port_range("dst_port",
					tb[min_port_type], tb[max_port_type]);

	if (!flower_port_range_attr_type(ip_proto, FLOWER_ENDPOINT_SRC,
					 &min_port_type, &max_port_type))
		flower_print_port_range("src_port",
					tb[min_port_type], tb[max_port_type]);

	//显示tcp flags
	flower_print_tcp_flags("tcp_flags", tb[TCA_FLOWER_KEY_TCP_FLAGS],
			       tb[TCA_FLOWER_KEY_TCP_FLAGS_MASK]);

	nl_type = flower_icmp_attr_type(eth_type, ip_proto,
					FLOWER_ICMP_FIELD_TYPE);
	nl_mask_type = flower_icmp_attr_mask_type(eth_type, ip_proto,
						  FLOWER_ICMP_FIELD_TYPE);
	if (nl_type >= 0 && nl_mask_type >= 0)
		flower_print_masked_u8("icmp_type", tb[nl_type],
				       tb[nl_mask_type], NULL);

	nl_type = flower_icmp_attr_type(eth_type, ip_proto,
					FLOWER_ICMP_FIELD_CODE);
	nl_mask_type = flower_icmp_attr_mask_type(eth_type, ip_proto,
						  FLOWER_ICMP_FIELD_CODE);
	if (nl_type >= 0 && nl_mask_type >= 0)
		flower_print_masked_u8("icmp_code", tb[nl_type],
				       tb[nl_mask_type], NULL);

	flower_print_ip4_addr("arp_sip", tb[TCA_FLOWER_KEY_ARP_SIP],
			     tb[TCA_FLOWER_KEY_ARP_SIP_MASK]);
	flower_print_ip4_addr("arp_tip", tb[TCA_FLOWER_KEY_ARP_TIP],
			     tb[TCA_FLOWER_KEY_ARP_TIP_MASK]);
	flower_print_arp_op("arp_op", tb[TCA_FLOWER_KEY_ARP_OP],
			    tb[TCA_FLOWER_KEY_ARP_OP_MASK]);
	flower_print_eth_addr("arp_sha", tb[TCA_FLOWER_KEY_ARP_SHA],
			      tb[TCA_FLOWER_KEY_ARP_SHA_MASK]);
	flower_print_eth_addr("arp_tha", tb[TCA_FLOWER_KEY_ARP_THA],
			      tb[TCA_FLOWER_KEY_ARP_THA_MASK]);

	flower_print_ip_addr("enc_dst_ip",
			     tb[TCA_FLOWER_KEY_ENC_IPV4_DST_MASK] ?
			     htons(ETH_P_IP) : htons(ETH_P_IPV6),
			     tb[TCA_FLOWER_KEY_ENC_IPV4_DST],
			     tb[TCA_FLOWER_KEY_ENC_IPV4_DST_MASK],
			     tb[TCA_FLOWER_KEY_ENC_IPV6_DST],
			     tb[TCA_FLOWER_KEY_ENC_IPV6_DST_MASK]);

	flower_print_ip_addr("enc_src_ip",
			     tb[TCA_FLOWER_KEY_ENC_IPV4_SRC_MASK] ?
			     htons(ETH_P_IP) : htons(ETH_P_IPV6),
			     tb[TCA_FLOWER_KEY_ENC_IPV4_SRC],
			     tb[TCA_FLOWER_KEY_ENC_IPV4_SRC_MASK],
			     tb[TCA_FLOWER_KEY_ENC_IPV6_SRC],
			     tb[TCA_FLOWER_KEY_ENC_IPV6_SRC_MASK]);

	flower_print_key_id("enc_key_id", tb[TCA_FLOWER_KEY_ENC_KEY_ID]);

	flower_print_port("enc_dst_port", tb[TCA_FLOWER_KEY_ENC_UDP_DST_PORT]);

	flower_print_ip_attr("enc_tos", tb[TCA_FLOWER_KEY_ENC_IP_TOS],
			    tb[TCA_FLOWER_KEY_ENC_IP_TOS_MASK]);
	flower_print_ip_attr("enc_ttl", tb[TCA_FLOWER_KEY_ENC_IP_TTL],
			    tb[TCA_FLOWER_KEY_ENC_IP_TTL_MASK]);
	flower_print_enc_opts("enc_opt", tb[TCA_FLOWER_KEY_ENC_OPTS],
			      tb[TCA_FLOWER_KEY_ENC_OPTS_MASK]);

	flower_print_matching_flags("ip_flags", FLOWER_IP_FLAGS,
				    tb[TCA_FLOWER_KEY_FLAGS],
				    tb[TCA_FLOWER_KEY_FLAGS_MASK]);

	close_json_object();

	if (tb[TCA_FLOWER_FLAGS]) {
		__u32 flags = rta_getattr_u32(tb[TCA_FLOWER_FLAGS]);

		if (flags & TCA_CLS_FLAGS_SKIP_HW)
			print_bool(PRINT_ANY, "skip_hw", "\n  skip_hw", true);
		if (flags & TCA_CLS_FLAGS_SKIP_SW)
			print_bool(PRINT_ANY, "skip_sw", "\n  skip_sw", true);

		//如果在hw中，则输出"in_hw"
		if (flags & TCA_CLS_FLAGS_IN_HW) {
			print_bool(PRINT_ANY, "in_hw", "\n  in_hw", true);

			if (tb[TCA_FLOWER_IN_HW_COUNT]) {
				__u32 count = rta_getattr_u32(tb[TCA_FLOWER_IN_HW_COUNT]);

				print_uint(PRINT_ANY, "in_hw_count",
					   " in_hw_count %u", count);
			}
		}
		else if (flags & TCA_CLS_FLAGS_NOT_IN_HW)
			print_bool(PRINT_ANY, "not_in_hw", "\n  not_in_hw", true);
	}

	if (tb[TCA_FLOWER_ACT])
		tc_print_action(f, tb[TCA_FLOWER_ACT], 0);

	return 0;
}

//flower对应的选项解析
struct filter_util flower_filter_util = {
	.id = "flower",
	.parse_fopt = flower_parse_opt,
	.print_fopt = flower_print_opt,
};
