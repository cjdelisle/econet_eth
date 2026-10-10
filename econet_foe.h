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

/* Shared L2 trailer: rewritten MAC/VLAN/PPPoE header for the bound flow. */
/*
 * __en75_ppe_foe_commit() byte-swaps each 32-bit word (cpu_to_le32). On a
 * big-endian host that swaps the two u16 halves of every two-u16 word, so
 * those pairs are declared reversed to land right in the LE table; on a
 * little-endian host cpu_to_le32 is a no-op and the natural order is correct.
 */
struct en75_foe_mac_info {
#ifdef CONFIG_CPU_BIG_ENDIAN
	u16 etype;
	u16 vlan1;
#else
	u16 vlan1;
	u16 etype;
#endif

	u32 dest_mac_hi;

#ifdef CONFIG_CPU_BIG_ENDIAN
	u16 dest_mac_lo;
	u16 vlan2;
#else
	u16 vlan2;
	u16 dest_mac_lo;
#endif

	u32 src_mac_hi;

#ifdef CONFIG_CPU_BIG_ENDIAN
	u16 src_mac_lo;
	u16 pppoe_id;
#else
	u16 pppoe_id;
	u16 src_mac_lo;
#endif
};

static_assert(sizeof(struct en75_foe_mac_info) == 20,
	       "v1 FOE mac_info must be 20 bytes");

struct en75_ipv4_tuple {
	u32 src_ip;
	u32 dest_ip;
	union {
		struct {
			u16 dest_port;
			u16 src_port;
		};
		struct {
			u8 protocol;
			u8 _pad[3]; /* fill with 0xa5a5a5 */
		};
		u32 ports;
	};
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

	u16 timestamp;
	u16 _rsv0[3];

	u32 udf_tsid;

	struct en75_foe_mac_info l2;
};

/* IPv6 3-tuple / 5-tuple route entry. */
struct en75_foe_ipv6 {
	u32 src_ip[4];
	u32 dest_ip[4];

	union {
		struct {
			u8 protocol;
			u8 _pad[3]; /* fill with 0xa5a5a5 */
		}; /* 3-tuple */
		struct {
			u16 dest_port;
			u16 src_port;
		}; /* 5-tuple */
		u32 ports;
	};

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
