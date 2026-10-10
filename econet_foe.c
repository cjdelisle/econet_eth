// SPDX-License-Identifier: GPL-2.0-only
/*
 * FOE (Flow Offload Engine) table-entry construction for the EcoNet EN751221
 * PPE.
 *
 * These helpers build a single 80-byte MediaTek "netsys v1" FOE entry in host
 * memory: the L3 tuple, the rewritten L2 header (MAC/VLAN/PPPoE) and the
 * information blocks that tell the engine how to forward a bound flow. They
 * mirror the mainline mtk_ppe entry builders (the v1 path only) so the field
 * encodings match the hardware the vendor hw_nat.ko drives.
 *
 * Everything here is pure: it only touches the caller's struct en75_foe_entry
 * and never the hardware. Committing a finished entry into the live FOE table
 * (timestamp, cache flush, table write) is done elsewhere.
 */
#include <linux/bitfield.h>
#include <linux/swab.h>
#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/unaligned.h>

#include "econet_ppe.h"

static int en75_get_ib1_pkt_type(u32 ib1)
{
	return FIELD_GET(EN75_FOE_IB1_PACKET_TYPE, ib1);
}

/*
 * The rewritten L2 header lives at a type-dependent offset: the bridge entry
 * carries it inline, the IPv6/dslite/6rd entries share the larger ipv6 body,
 * and the IPv4 NAT/route entries use the ipv4 body.
 */
static struct en75_foe_mac_info *en75_foe_entry_l2(struct en75_foe_entry *entry)
{
	int type = en75_get_ib1_pkt_type(entry->ib1);

	if (type == EN75_PPE_PKT_TYPE_BRIDGE)
		return &entry->bridge.l2;

	if (type >= EN75_PPE_PKT_TYPE_IPV4_DSLITE)
		return &entry->ipv6.l2;

	return &entry->ipv4.l2;
}

static u32 *en75_foe_entry_ib2(struct en75_foe_entry *entry)
{
	int type = en75_get_ib1_pkt_type(entry->ib1);

	if (type == EN75_PPE_PKT_TYPE_BRIDGE)
		return &entry->bridge.ib2;

	if (type >= EN75_PPE_PKT_TYPE_IPV4_DSLITE)
		return &entry->ipv6.ib2;

	return &entry->ipv4.ib2;
}

/**
 * en75_foe_entry_prepare() - initialise a FOE entry for a flow.
 * @entry:    entry to fill (fully overwritten).
 * @type:     EN75_PPE_PKT_TYPE_* of the flow.
 * @l4proto:  IP L4 protocol (selects the UDP information bit).
 * @pse_port: PSE egress port the bound flow is forwarded to (etx_fport).
 * @src_mac:  rewritten source MAC (the router's egress MAC).
 * @dest_mac: rewritten destination MAC (the next hop).
 *
 * Sets the entry to the BIND state with the L2 rewrite header and the
 * information blocks. The L3 tuple is filled in afterwards with the
 * en75_foe_entry_set_*_tuple() helpers.
 */
int en75_foe_entry_prepare(struct en75_foe_entry *entry, int type, int l4proto,
			   u8 pse_port, const u8 *src_mac, const u8 *dest_mac)
{
	struct en75_foe_mac_info *l2;
	u32 ports_pad, val;

	memset(entry, 0, sizeof(*entry));

	entry->ib1 = FIELD_PREP(EN75_FOE_IB1_STATE, EN75_FOE_STATE_BIND) |
		     FIELD_PREP(EN75_FOE_IB1_PACKET_TYPE, type) |
		     FIELD_PREP(EN75_FOE_IB1_UDP, l4proto == IPPROTO_UDP) |
		     EN75_FOE_IB1_BIND_CACHE | EN75_FOE_IB1_BIND_TTL;

	val = FIELD_PREP(EN75_FOE_IB2_DEST_PORT, pse_port) |
	      FIELD_PREP(EN75_FOE_IB2_PORT_MG, 0x3f) |
	      FIELD_PREP(EN75_FOE_IB2_PORT_AG, 0x1f);

	if (is_multicast_ether_addr(dest_mac))
		val |= EN75_FOE_IB2_MULTICAST;

	/* Route entries keep no L4 ports; the field is poisoned like the
	 * vendor entry so a partial match never aliases a real port pair.
	 * 0xa5 is an arbitrary sentinel byte (same idea as 0xdeadbeef),
	 * repeated across all three pad bytes since only the low byte
	 * (l4proto) is ever read back.
	 */
	ports_pad = 0xa5a5a500 | (l4proto & 0xff);
	if (type == EN75_PPE_PKT_TYPE_IPV4_ROUTE)
		entry->ipv4.orig.ports = ports_pad;
	if (type == EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T)
		entry->ipv6.ports = ports_pad;

	if (type == EN75_PPE_PKT_TYPE_BRIDGE) {
		ether_addr_copy(entry->bridge.src_mac, src_mac);
		ether_addr_copy(entry->bridge.dest_mac, dest_mac);
		entry->bridge.ib2 = val;
		l2 = &entry->bridge.l2;
	} else if (type >= EN75_PPE_PKT_TYPE_IPV4_DSLITE) {
		entry->ipv6.ib2 = val;
		l2 = &entry->ipv6.l2;
	} else {
		entry->ipv4.ib2 = val;
		l2 = &entry->ipv4.l2;
	}

	l2->dest_mac_hi = get_unaligned_be32(dest_mac);
	l2->dest_mac_lo = get_unaligned_be16(dest_mac + 4);
	l2->src_mac_hi = get_unaligned_be32(src_mac);
	l2->src_mac_lo = get_unaligned_be16(src_mac + 4);

	if (type >= EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T)
		l2->etype = ETH_P_IPV6;
	else
		l2->etype = ETH_P_IP;

	return 0;
}

/**
 * en75_foe_entry_set_pse_port() - override the egress PSE port.
 *
 * Updates only the DEST_PORT field of ib2, leaving the rest of the entry as
 * prepared. Used when the egress port is decided after en75_foe_entry_prepare().
 */
int en75_foe_entry_set_pse_port(struct en75_foe_entry *entry, u8 port)
{
	u32 *ib2 = en75_foe_entry_ib2(entry);

	*ib2 &= ~EN75_FOE_IB2_DEST_PORT;
	*ib2 |= FIELD_PREP(EN75_FOE_IB2_DEST_PORT, port);

	return 0;
}

/**
 * en75_foe_entry_set_ipv4_tuple() - set one side of an IPv4 flow's L3 tuple.
 * @egress: false for the original (match) tuple, true for the NAT'd (rewrite)
 *          tuple. The rewrite tuple only exists for HNAPT entries.
 *
 * Addresses and ports are passed in network byte order and stored in host
 * order, as the hardware expects. Route entries store only the addresses.
 */
int en75_foe_entry_set_ipv4_tuple(struct en75_foe_entry *entry, bool egress,
				  __be32 src_addr, __be16 src_port,
				  __be32 dest_addr, __be16 dest_port)
{
	int type = en75_get_ib1_pkt_type(entry->ib1);
	struct en75_ipv4_tuple *t;

	switch (type) {
	case EN75_PPE_PKT_TYPE_IPV4_HNAPT:
		if (egress) {
			t = &entry->ipv4.new;
			break;
		}
		fallthrough;
	case EN75_PPE_PKT_TYPE_IPV4_DSLITE:
	case EN75_PPE_PKT_TYPE_IPV4_ROUTE:
		t = &entry->ipv4.orig;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	/* Build the tuple in host byte order. __en75_ppe_foe_commit() converts
	 * the whole entry to the little-endian layout the engine reads.
	 */
	t->src_ip = be32_to_cpu(src_addr);
	t->dest_ip = be32_to_cpu(dest_addr);

	if (type == EN75_PPE_PKT_TYPE_IPV4_ROUTE)
		return 0;

	t->ports = ((u32)be16_to_cpu(src_port) << 16) | be16_to_cpu(dest_port);

	return 0;
}

/**
 * en75_foe_entry_set_ipv6_tuple() - set an IPv6 route flow's L3 tuple.
 *
 * Handles the 3-tuple (addresses only) and 5-tuple (addresses + ports)
 * route types. dslite/6rd tunnel entries are not yet supported.
 */
int en75_foe_entry_set_ipv6_tuple(struct en75_foe_entry *entry,
				  __be32 *src_addr, __be16 src_port,
				  __be32 *dest_addr, __be16 dest_port)
{
	int type = en75_get_ib1_pkt_type(entry->ib1);
	int i;

	switch (type) {
	case EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T:
		entry->ipv6.ports = ((u32)be16_to_cpu(src_port) << 16) |
				    be16_to_cpu(dest_port);
		fallthrough;
	case EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T:
		for (i = 0; i < 4; i++) {
			entry->ipv6.src_ip[i] = be32_to_cpu(src_addr[i]);
			entry->ipv6.dest_ip[i] = be32_to_cpu(dest_addr[i]);
		}
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	return 0;
}

/**
 * en75_foe_entry_set_vlan() - push a VLAN tag onto the rewritten L2 header.
 *
 * Up to two stacked tags are supported. The VLAN-layer count in ib1 tracks how
 * many tags are present; the second tag reuses the etype-as-protocol slot of
 * the first, matching the hardware's egress encoding.
 */
int en75_foe_entry_set_vlan(struct en75_foe_entry *entry, int vid)
{
	struct en75_foe_mac_info *l2 = en75_foe_entry_l2(entry);

	switch (FIELD_GET(EN75_FOE_IB1_BIND_VLAN_LAYER, entry->ib1)) {
	case 0:
		entry->ib1 |= EN75_FOE_IB1_BIND_VLAN_TAG |
			      FIELD_PREP(EN75_FOE_IB1_BIND_VLAN_LAYER, 1);
		l2->vlan1 = vid;
		return 0;
	case 1:
		if (!(entry->ib1 & EN75_FOE_IB1_BIND_VLAN_TAG)) {
			l2->vlan1 = vid;
			l2->etype |= BIT(8);
		} else {
			l2->vlan2 = vid;
			entry->ib1 += FIELD_PREP(EN75_FOE_IB1_BIND_VLAN_LAYER, 1);
		}
		return 0;
	default:
		return -ENOSPC;
	}
}

/**
 * en75_foe_entry_set_pppoe() - mark the rewritten L2 header as PPPoE.
 *
 * Adds the PPPoE session id and flags the entry so the engine inserts a PPPoE
 * session header on egress.
 */
int en75_foe_entry_set_pppoe(struct en75_foe_entry *entry, int sid)
{
	struct en75_foe_mac_info *l2 = en75_foe_entry_l2(entry);

	if (!(entry->ib1 & EN75_FOE_IB1_BIND_VLAN_LAYER) ||
	    (entry->ib1 & EN75_FOE_IB1_BIND_VLAN_TAG))
		l2->etype = ETH_P_PPP_SES;

	entry->ib1 |= EN75_FOE_IB1_BIND_PPPOE;
	l2->pppoe_id = sid;

	return 0;
}

/**
 * en75_foe_entry_hash() - FOE table index for an entry.
 *
 * Reproduces the vendor/mainline v1 hash so a flow lands in the same bucket
 * the engine searches for its packets. Only the original (ingress) tuple
 * participates. With hash_offset 2 the low bit selects one of two slots that
 * share a bucket.
 */
u32 en75_foe_entry_hash(struct en75_foe_entry *entry)
{
	u32 hv1, hv2, hv3, hash;

	switch (en75_get_ib1_pkt_type(entry->ib1)) {
	case EN75_PPE_PKT_TYPE_IPV4_ROUTE:
	case EN75_PPE_PKT_TYPE_IPV4_HNAPT:
		hv1 = entry->ipv4.orig.ports;
		hv2 = entry->ipv4.orig.dest_ip;
		hv3 = entry->ipv4.orig.src_ip;
		break;
	case EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T:
	case EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T:
		hv1 = entry->ipv6.src_ip[3] ^ entry->ipv6.dest_ip[3];
		hv1 ^= entry->ipv6.ports;
		hv2 = entry->ipv6.src_ip[2] ^ entry->ipv6.dest_ip[2];
		hv2 ^= entry->ipv6.dest_ip[0];
		hv3 = entry->ipv6.src_ip[1] ^ entry->ipv6.dest_ip[1];
		hv3 ^= entry->ipv6.src_ip[0];
		break;
	default:
		WARN_ON_ONCE(1);
		return EN75_PPE_ENTRIES - 1;
	}

	hash = (hv1 & hv2) | ((~hv1) & hv3);
	hash = (hash >> 24) | ((hash & 0xffffff) << 8);
	hash ^= hv1 ^ hv2 ^ hv3;
	hash ^= hash >> 16;
	hash <<= ffs(EN75_PPE_HASH_OFFSET) - 1;
	hash &= EN75_PPE_ENTRIES - 1;

	return hash;
}
