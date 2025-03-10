// SPDX-License-Identifier: MulanPSL-2.0

#include "net_utils.h"
#include "arg_parse.h"

#define _LINUX_IN_H
#include <netinet/in.h>

typedef struct {
	char *name;
	int val;
} proto_item_t;

#define for_each_protos(protos, size, i, item)	\
	for (i = 0, item = protos; i < size;	\
	     i++, item = protos + i)

static proto_item_t l3_protos[] = {
	{ "loop",	0x0060 },
	{ "pup",	0x0200 },
	{ "pupat",	0x0201 },
	{ "tsn",	0x22F0 },
	{ "erspan2",	0x22EB },
	{ "ip",		0x0800 },
	{ "x25",	0x0805 },
	{ "arp",	0x0806 },
	{ "bpq",	0x08FF },
	{ "ieeepup",	0x0a00 },
	{ "ieeepupat",	0x0a01 },
	{ "batman",	0x4305 },
	{ "dec",	0x6000 },
	{ "dna_dl",	0x6001 },
	{ "dna_rc",	0x6002 },
	{ "dna_rt",	0x6003 },
	{ "lat",	0x6004 },
	{ "diag",	0x6005 },
	{ "cust",	0x6006 },
	{ "sca",	0x6007 },
	{ "teb",	0x6558 },
	{ "rarp",	0x8035 },
	{ "atalk",	0x809B },
	{ "aarp",	0x80F3 },
	{ "8021q",	0x8100 },
	{ "erspan",	0x88BE },
	{ "ipx",	0x8137 },
	{ "ipv6",	0x86DD },
	{ "pause",	0x8808 },
	{ "slow",	0x8809 },
	{ "wccp",	0x883E },
};

static proto_item_t l4_protos[] = {
	{ "icmp",	1 },
	{ "igmp",	2 },
	{ "ipip",	4 },
	{ "tcp",	6 },
	{ "egp",	8 },
	{ "pup",	12 },
	{ "udp",	17 },
	{ "idp",	22 },
	{ "tp",		29 },
	{ "dccp",	33 },
	{ "ipv6",	41 },
	{ "rsvp",	46 },
	{ "gre",	47 },
	{ "esp",	50 },
	{ "ah",		51 },
	{ "mtp",	92 },
	{ "beetph",	94 },
	{ "encap",	98 },
	{ "pim",	103 },
	{ "comp",	108 },
	{ "sctp",	132 },
	{ "udplite",	136 },
	{ "mpls",	137 },
	{ "raw",	255 },
};

char *l4_proto_names[] = {
	[1]	= "ICMP",
	[2]	= "IGMP",
	[4]	= "IPIP",
	[6]	= "TCP",
	[8]	= "EGP",
	[12]	= "PUP",
	[17]	= "UDP",
	[22]	= "IDP",
	[29]	= "TP",
	[33]	= "DCCP",
	[41]	= "IPV6",
	[46]	= "RSVP",
	[47]	= "GRE",
	[50]	= "ESP",
	[51]	= "AH",
	[92]	= "MTP",
	[94]	= "BEETPH",
	[98]	= "ENCAP",
	[103]	= "PIM",
	[108]	= "COMP",
	[132]	= "SCTP",
	[136]	= "UDPLITE",
	[137]	= "MPLS",
	[255]	= "RAW",
};

static proto_item_t *proto_search(proto_item_t *protos, int size,
				  char *name)
{
	proto_item_t *item;
	int i = 0;

	for_each_protos(protos, size, i, item) {
		if (strcmp(item->name, name) == 0)
			return item;
	}
	return NULL;
}

int l3proto2i(char *proto, int *dest)
{
	proto_item_t *item = proto_search(l3_protos, ARRAY_SIZE(l3_protos),
					  proto);
	if (item) {
		*dest = item->val;
		return 0;
	}
	return -1;
}

int l4proto2i(char *proto, int *dest)
{
	proto_item_t *item = proto_search(l4_protos, ARRAY_SIZE(l4_protos),
					  proto);
	if (item) {
		*dest = item->val;
		return 0;
	}
	return -1;
}

int proto2i(char *proto, int *dest)
{
	if (!l3proto2i(proto, dest))
		return 3;
	if (!l4proto2i(proto, dest))
		return 4;
	return 0;
}

void i2ipv6(char *dest, __u8 ip[])
{
	int i = 0, offset = 0;

	for (;  i < 16; i += 2) {
		if (ip[i] || ip[i + 1])
			offset += sprintf(dest + offset, "%02x%02x:",
					  ip[i], ip[i + 1]);
		else
			offset += sprintf(dest + offset, ":");
	}
	*(dest + offset - 1) = '\0';
}

int ipv6toi(char *ip, __u8 *dest)
{
	u16 *c = (u16 *)dest;
	u32 t[8] = {}, i = 0;

	if (sscanf(ip, "%x:%x:%x:%x:%x:%x:%x:%x", t, t + 1, t + 2,
		   t + 3, t + 4, t + 5, t + 6, t + 7) == 8)
		goto is_valid;

	memset(t, 0, sizeof(t));
	if (sscanf(ip, "%x::%x:%x:%x:%x", t, t + 4, t + 5, t + 6,
		   t + 7) == 5)
		goto is_valid;

	return -1;

is_valid:
	for (i = 0; i < 8; i++)
		c[i] = htons(t[i]);
	return 0;
}
