/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hardware FOE (Flow Offload Engine) table entry layout for the EcoNet
 * EN751221 PPE.
 *
 * The entry is the MediaTek "netsys v1" 80-byte format (mt7621 class). The
 * field layout below mirrors mainline mtk_ppe.h so the entry construction
 * helpers can be ported with matching offsets. Unlike mainline the mac_info
 * here carries only the v1 fields (20 bytes); that is what makes the IPv6
 * 5-tuple variant come out to exactly 80 bytes.
 */
#ifndef ECONET_FOE_H
#define ECONET_FOE_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/if_ether.h>
#include <linux/types.h>

/* Information block 1 (entry word 0). */
#define EN75_FOE_IB1_UNBIND_TIMESTAMP	GENMASK(7, 0)
#define EN75_FOE_IB1_UNBIND_PACKETS	GENMASK(23, 8)
#define EN75_FOE_IB1_UNBIND_PREBIND	BIT(24)

#define EN75_FOE_IB1_BIND_TIMESTAMP	GENMASK(14, 0)
#define EN75_FOE_IB1_BIND_KEEPALIVE	BIT(15)
#define EN75_FOE_IB1_BIND_VLAN_LAYER	GENMASK(18, 16)
#define EN75_FOE_IB1_BIND_PPPOE		BIT(19)
#define EN75_FOE_IB1_BIND_VLAN_TAG	BIT(20)
#define EN75_FOE_IB1_BIND_PKT_SAMPLE	BIT(21)
#define EN75_FOE_IB1_BIND_CACHE		BIT(22)
#define EN75_FOE_IB1_BIND_TUNNEL_DECAP	BIT(23)
#define EN75_FOE_IB1_BIND_TTL		BIT(24)

#define EN75_FOE_IB1_PACKET_TYPE	GENMASK(27, 25)
#define EN75_FOE_IB1_STATE		GENMASK(29, 28)
#define EN75_FOE_IB1_UDP		BIT(30)
#define EN75_FOE_IB1_STATIC		BIT(31)

enum {
	EN75_FOE_STATE_INVALID,
	EN75_FOE_STATE_UNBIND,
	EN75_FOE_STATE_BIND,
	EN75_FOE_STATE_FIN,
};

enum {
	EN75_PPE_PKT_TYPE_IPV4_HNAPT		= 0,
	EN75_PPE_PKT_TYPE_IPV4_ROUTE		= 1,
	EN75_PPE_PKT_TYPE_BRIDGE		= 2,
	EN75_PPE_PKT_TYPE_IPV4_DSLITE		= 3,
	EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T		= 4,
	EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T		= 5,
	EN75_PPE_PKT_TYPE_IPV6_6RD		= 7,
};

/* Two FOE entries share a hash bucket (vendor gPpeHashMode / mtk hash_offset). */
#define EN75_PPE_HASH_OFFSET		2

/* Information block 2. */
#define EN75_FOE_IB2_QID		GENMASK(3, 0)
#define EN75_FOE_IB2_PSE_QOS		BIT(4)
#define EN75_FOE_IB2_DEST_PORT		GENMASK(7, 5)
#define EN75_FOE_IB2_MULTICAST		BIT(8)
#define EN75_FOE_IB2_WDMA_QID2		GENMASK(13, 12)
#define EN75_FOE_IB2_WDMA_DEVIDX	BIT(16)
#define EN75_FOE_IB2_WDMA_WINFO		BIT(17)
#define EN75_FOE_IB2_PORT_MG		GENMASK(17, 12)
#define EN75_FOE_IB2_PORT_AG		GENMASK(23, 18)
#define EN75_FOE_IB2_DSCP		GENMASK(31, 24)

/* Shared L2 trailer: rewritten MAC/VLAN/PPPoE header for the bound flow.
 *
 * Each two-u16 wire pair (vlan/etype, vlan/mac-lo, pppoe/mac-lo) is kept as
 * one plain u32 word with GENMASK/FIELD_PREP accessors instead of two named
 * u16 struct members. That makes the layout host-endian-agnostic by
 * construction: __en75_ppe_foe_commit() byte-swaps each 32-bit word
 * (cpu_to_le32) on its way into the LE table, and FIELD_PREP/FIELD_GET
 * always operate on the value, never on its in-memory byte order, so there
 * is nothing left for a big-endian host to get wrong.
 */
#define EN75_MAC_INFO_VLAN1		GENMASK(15, 0)
#define EN75_MAC_INFO_ETYPE		GENMASK(31, 16)
#define EN75_MAC_INFO_VLAN2		GENMASK(15, 0)
#define EN75_MAC_INFO_DEST_MAC_LO	GENMASK(31, 16)
#define EN75_MAC_INFO_PPPOE_ID		GENMASK(15, 0)
#define EN75_MAC_INFO_SRC_MAC_LO	GENMASK(31, 16)

struct en75_foe_mac_info {
	u32 w0; /* EN75_MAC_INFO_VLAN1 | EN75_MAC_INFO_ETYPE */
	u32 dest_mac_hi;
	u32 w1; /* EN75_MAC_INFO_VLAN2 | EN75_MAC_INFO_DEST_MAC_LO */
	u32 src_mac_hi;
	u32 w2; /* EN75_MAC_INFO_PPPOE_ID | EN75_MAC_INFO_SRC_MAC_LO */
};

static_assert(sizeof(struct en75_foe_mac_info) == 20,
	       "v1 FOE mac_info must be 20 bytes");

/* Poison pattern for an L4-port field the hardware never reads (route
 * entries, bridge entries): matches the vendor entry's sentinel bytes so a
 * partial match can never alias a real port pair. The low byte is the only
 * one ever consulted (en75_foe_entry_hash()'s l4proto use); the other three
 * stay 0xa5 for parity with the vendor table.
 */
#define EN75_PPE_TUPLE_PAD	0xa5a5a500

struct en75_ipv4_tuple {
	u32 src_ip;
	u32 dest_ip;
	u32 ports; /* HNAPT/NAPT: (src_port << 16) | dest_port, host order
		    * before __en75_ppe_foe_commit(). Route entries carry no
		    * ports: EN75_PPE_TUPLE_PAD | l4proto instead.
		    */
};

/* L2 bridge offload entry. */
struct en75_foe_bridge {
	u8 dest_mac[ETH_ALEN];
	u8 src_mac[ETH_ALEN];
	u16 vlan;

	u32 ib2;

	struct en75_foe_mac_info l2;
};

/* IPv4 NAT / NAPT entry. */
struct en75_foe_ipv4 {
	struct en75_ipv4_tuple orig;

	u32 ib2;

	struct en75_ipv4_tuple new;

	/* Per-entry timestamp: AFAIK not used by this hardware (aging instead
	 * reads ib1's own BIND_TIMESTAMP bits via en75_ppe_timestamp()); kept
	 * reserved to match the mainline mtk_ppe v1 layout's word count.
	 */
	u16 _rsv_ts;
	u16 _rsv0[3];

	/* Unused by this driver; no current setter. Byte layout per upstream
	 * review: act_dp [7:0], tsid [15:8], channel [23:16] (useful for
	 * traffic shaping), reserved [31:24].
	 */
#define EN75_FOE_UDF_ACT_DP	GENMASK(7, 0)
#define EN75_FOE_UDF_TSID	GENMASK(15, 8)
#define EN75_FOE_UDF_CHANNEL	GENMASK(23, 16)
	u32 udf_tsid;

	struct en75_foe_mac_info l2;
};

/* IPv6 3-tuple / 5-tuple route entry. */
struct en75_foe_ipv6 {
	u32 src_ip[4];
	u32 dest_ip[4];

	u32 ports; /* 5-tuple: (src_port << 16) | dest_port, host order before
		    * __en75_ppe_foe_commit(). 3-tuple: EN75_PPE_TUPLE_PAD |
		    * l4proto instead (see en75_ipv4_tuple::ports).
		    */

	u32 _rsv[3];

	u32 udf;

	u32 ib2;
	struct en75_foe_mac_info l2;
};

/* One 80-byte hardware FOE table entry. */
struct en75_foe_entry {
	u32 ib1;
	union {
		struct en75_foe_bridge	bridge;
		struct en75_foe_ipv4	ipv4;
		struct en75_foe_ipv6	ipv6;
		u32			data[19];
	};
};

static_assert(sizeof(struct en75_foe_entry) == 80,
	       "FOE entry must be 80 bytes (MTK v1 layout)");

/* FOE table-entry construction (econet_foe.c). Pure host-memory helpers. */
int en75_foe_entry_prepare(struct en75_foe_entry *entry, int type, int l4proto,
			   u8 pse_port, const u8 *src_mac, const u8 *dest_mac);
int en75_foe_entry_set_pse_port(struct en75_foe_entry *entry, u8 port);
int en75_foe_entry_set_ipv4_tuple(struct en75_foe_entry *entry, bool egress,
				  __be32 src_addr, __be16 src_port,
				  __be32 dest_addr, __be16 dest_port);
int en75_foe_entry_set_ipv6_tuple(struct en75_foe_entry *entry,
				  __be32 *src_addr, __be16 src_port,
				  __be32 *dest_addr, __be16 dest_port);
int en75_foe_entry_set_vlan(struct en75_foe_entry *entry, int vid);
int en75_foe_entry_set_pppoe(struct en75_foe_entry *entry, int sid);
u32 en75_foe_entry_hash(struct en75_foe_entry *entry);

#endif /* ECONET_FOE_H */
