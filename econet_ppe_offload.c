// SPDX-License-Identifier: GPL-2.0-only
/*
 * nf_flowtable / TC flower offload glue for the EcoNet PPE (EN751221/EN7528).
 *
 * Ported from the mainline mtk_ppe_offload.c (Felix Fietkau), trimmed to the
 * netsys-v1 FOE table format both chips implement, with no netsys-v2 fields.
 * Neither DSA nor WED (present on EN7528, not on EN751221) are wired up yet.
 * A flow offered by the kernel flowtable is translated into an 80-byte FOE
 * entry with the econet_foe.c builders and committed into the live table.
 *
 * The EN751221 FOE lookup hash is a bespoke CRC the driver cannot yet
 * reproduce, so a software-computed slot would never be searched by the
 * engine. Instead the commit is deferred: REPLACE only stages the prebuilt
 * entry (keyed by its ingress 5-tuple); when a real packet for that flow
 * misses (UN_HIT) the RX path calls en75_ppe_offload_rx_commit() with the
 * slot the hardware itself reported in the descriptor, and the entry is
 * published there - exactly where the engine will look it up.
 */
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_vlan.h>
#include <linux/mutex.h>
#include <linux/rhashtable.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "econet_ppe.h"

struct en75_flow_data {
	struct ethhdr eth;

	union {
		struct {
			__be32 src_addr;
			__be32 dst_addr;
		} v4;

		struct {
			struct in6_addr src_addr;
			struct in6_addr dst_addr;
		} v6;
	};

	__be16 src_port;
	__be16 dst_port;

	u16 vlan_in;

	struct {
		struct {
			u16 id;
			__be16 proto;
		} vlans[2];
		u8 num;
	} vlan;
	struct {
		u16 sid;
		u8 num;
	} pppoe;
};

/* Ingress 5-tuple key, in network byte order; fully zeroed before use so
 * the trailing pad is deterministic for the rhashtable memcmp.
 */
struct en75_flow_tuple {
	__be32 src[4];		/* v4 in [0] */
	__be32 dst[4];
	__be16 sport;
	__be16 dport;
	u8 is_v6;
	u8 l4proto;
	u8 pad[2];
};

enum en75_flow_state {
	EN75_FLOW_INVALID = 0,	/* should never exist; catches a missed init */
	EN75_FLOW_PENDING,	/* staged, awaiting the hardware slot */
	EN75_FLOW_COMMITTED,	/* published at ->hash */
	EN75_FLOW_DEAD,		/* torn down; awaiting RCU free */
};

struct en75_flow_entry {
	struct rhash_head node;		/* keyed by cookie */
	struct rhash_head tuple_node;	/* keyed by tuple */
	struct rcu_head rcu;
	unsigned long cookie;
	struct en75_flow_tuple tuple;
	struct en75_foe_entry foe;	/* prebuilt; committed lazily by RX */
	u32 hash;			/* slot, valid once COMMITTED */
	enum en75_flow_state state;
	unsigned long last_scan;	/* jiffies of last engine-slot scan */
};

static const struct rhashtable_params en75_flow_ht_params = {
	.head_offset = offsetof(struct en75_flow_entry, node),
	.key_offset = offsetof(struct en75_flow_entry, cookie),
	.key_len = sizeof(unsigned long),
	.automatic_shrinking = true,
};

static const struct rhashtable_params en75_tuple_ht_params = {
	.head_offset = offsetof(struct en75_flow_entry, tuple_node),
	.key_offset = offsetof(struct en75_flow_entry, tuple),
	.key_len = sizeof(struct en75_flow_tuple),
	.automatic_shrinking = true,
};

static void
en75_flow_tuple_from_data(struct en75_flow_tuple *t, u16 addr_type,
			  u8 l4proto, const struct en75_flow_data *data)
{
	memset(t, 0, sizeof(*t));
	t->l4proto = l4proto;
	t->sport = data->src_port;
	t->dport = data->dst_port;
	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		t->is_v6 = 1;
		memcpy(t->src, &data->v6.src_addr, sizeof(t->src));
		memcpy(t->dst, &data->v6.dst_addr, sizeof(t->dst));
	} else {
		t->src[0] = data->v4.src_addr;
		t->dst[0] = data->v4.dst_addr;
	}
}

/* Parse the ingress 5-tuple straight from a received frame (post-VLAN). */
static void
en75_flow_tuple_from_skb(struct en75_flow_tuple *t, struct sk_buff *skb)
{
	const u8 *data = skb->data;
	unsigned int l2 = ETH_HLEN;
	__be16 proto;

	memset(t, 0, sizeof(*t));

	if (skb->len < ETH_HLEN)
		return;
	proto = ((const struct ethhdr *)data)->h_proto;
	while (proto == htons(ETH_P_8021Q) || proto == htons(ETH_P_8021AD)) {
		if (skb->len < l2 + VLAN_HLEN)
			return;
		proto = *(const __be16 *)(data + l2 + 2);
		l2 += VLAN_HLEN;
	}

	/* PPPoE session frames (WAN side): a 6-byte session header followed by a
	 * 2-byte PPP protocol id, then the inner IP packet. The engine parses
	 * these and learns the inner 5-tuple, so the staged reply-direction entry
	 * is keyed on the *decapsulated* tuple. Without this the download (reply)
	 * direction never matches and never binds, which is the bulk of traffic. */
	if (proto == htons(ETH_P_PPP_SES)) {
		__be16 ppp;

		if (skb->len < l2 + 8)
			return;
		ppp = *(const __be16 *)(data + l2 + 6);
		l2 += 8;
		if (ppp == htons(0x0021))		/* PPP_IP */
			proto = htons(ETH_P_IP);
		else if (ppp == htons(0x0057))		/* PPP_IPV6 */
			proto = htons(ETH_P_IPV6);
		else
			return;
	}

	if (proto == htons(ETH_P_IP)) {
		const struct iphdr *iph = (const struct iphdr *)(data + l2);
		const __be16 *ports;
		unsigned int ihl;

		if (skb->len < l2 + sizeof(*iph))
			return;
		ihl = iph->ihl * 4;
		if (ihl < sizeof(*iph) || skb->len < l2 + ihl + 4)
			return;
		if (iph->protocol != IPPROTO_TCP &&
		    iph->protocol != IPPROTO_UDP)
			return;
		ports = (const __be16 *)((const u8 *)iph + ihl);
		t->l4proto = iph->protocol;
		t->src[0] = iph->saddr;
		t->dst[0] = iph->daddr;
		t->sport = ports[0];
		t->dport = ports[1];
	} else if (proto == htons(ETH_P_IPV6)) {
		const struct ipv6hdr *ip6 =
			(const struct ipv6hdr *)(data + l2);
		const __be16 *ports = (const __be16 *)(ip6 + 1);

		if (skb->len < l2 + sizeof(*ip6) + 4)
			return;
		if (ip6->nexthdr != IPPROTO_TCP &&
		    ip6->nexthdr != IPPROTO_UDP)
			return;
		t->is_v6 = 1;
		t->l4proto = ip6->nexthdr;
		memcpy(t->src, &ip6->saddr, sizeof(t->src));
		memcpy(t->dst, &ip6->daddr, sizeof(t->dst));
		t->sport = ports[0];
		t->dport = ports[1];
	}
}

/*
 * Apply one eth-header rewrite action into the 14-byte L2 scratch buffer.
 *
 * nf_flow_table_offload encodes the new MACs as fixed actions whose value/
 * mask are host u32s laid out for a LITTLE-ENDIAN CPU. On LE the upstream mtk
 * byte-copy reproduces that directly; on big-endian the two 2-byte halves
 * (dst MAC[4:6] and src MAC[0:2]) come out zero, so there decode each action
 * by its mask and place the correct native bytes explicitly.
 */
static void
en75_flow_offload_mangle_eth(const struct flow_action_entry *act, void *eth)
{
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
		const u8 *vb = (const u8 *)&act->mangle.val;	/* native (BE) bytes */
		u8 *e = (u8 *)eth;

		switch (act->mangle.mask) {
		case 0x00000000:	/* dst[0:4] (off 0) or src[2:6] (off 8) */
			if (act->mangle.offset <= 8)
				memcpy(e + act->mangle.offset, vb, 4);
			break;
		case 0xffff0000:	/* dst MAC[4:6] -> eth[4:6] */
			memcpy(e + 4, vb + 2, 2);
			break;
		case 0x0000ffff:	/* src MAC[0:2] -> eth[6:8] */
			memcpy(e + 6, vb, 2);
			break;
		default:
			break;
		}
	} else {
		void *dest = eth + act->mangle.offset;
		const void *src = &act->mangle.val;

		if (act->mangle.offset > 8)
			return;

		if (act->mangle.mask == 0xffff) {
			src += 2;
			dest += 2;
		}

		memcpy(dest, src, act->mangle.mask ? 2 : 4);
	}
}

/*
 * act->mangle.mask marks the bits that are PRESERVED, not the bits being
 * written (standard pedit/flow_action convention: new = (old & mask) |
 * (val & ~mask)) - so mask == ~htonl(0xffff) (preserve the upper/src-port
 * half) means this mangle is replacing dst_port, and vice versa. val is
 * ntohl()'d once up front so val's high 16 bits are always src_port and
 * the low 16 are always dst_port, regardless of which half the mask says
 * to replace.
 */
static int
en75_flow_mangle_ports(const struct flow_action_entry *act,
		       struct en75_flow_data *data)
{
	u32 val = ntohl(act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if (act->mangle.mask == ~htonl(0xffff))
			data->dst_port = cpu_to_be16(val);
		else
			data->src_port = cpu_to_be16(val >> 16);
		break;
	case 2:
		data->dst_port = cpu_to_be16(val);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int
en75_flow_mangle_ipv4(const struct flow_action_entry *act,
		      struct en75_flow_data *data)
{
	__be32 *dest;

	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		dest = &data->v4.src_addr;
		break;
	case offsetof(struct iphdr, daddr):
		dest = &data->v4.dst_addr;
		break;
	default:
		return -EINVAL;
	}

	memcpy(dest, &act->mangle.val, sizeof(u32));

	return 0;
}

static int
en75_flow_set_ipv4_addr(struct en75_foe_entry *foe, struct en75_flow_data *data,
			bool egress)
{
	return en75_foe_entry_set_ipv4_tuple(foe, egress,
					     data->v4.src_addr, data->src_port,
					     data->v4.dst_addr, data->dst_port);
}

static int
en75_ppe_offload_replace(struct en75_ppe *ppe, struct flow_cls_offload *f)
{
	struct en75_flow_tuple keytuple;
	struct flow_action_entry *act;
	struct en75_flow_entry *entry;
	struct en75_flow_data data = {};
	struct en75_foe_entry foe;
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct net_device *odev = NULL;
	int offload_type = 0;
	u16 addr_type = 0;
	u8 l4proto = 0;
	int err = 0;
	int i;

	if (rhashtable_lookup(&ppe->flow_table, &f->cookie, en75_flow_ht_params))
		return -EEXIST;

	/* The flowtable always tags the rule with the ingress meta key. */
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		ppe->diag[EN75_DIAG_REPL_REJ_BASIC]++;
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;

		if (flow_rule_has_control_flags(match.mask->flags,
						f->common.extack)) {
			ppe->diag[EN75_DIAG_REPL_REJ_BASIC]++;
			return -EOPNOTSUPP;
		}
	} else {
		ppe->diag[EN75_DIAG_REPL_REJ_BASIC]++;
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		l4proto = match.key->ip_proto;
	} else {
		ppe->diag[EN75_DIAG_REPL_REJ_BASIC]++;
		return -EOPNOTSUPP;
	}

	switch (addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		offload_type = EN75_PPE_PKT_TYPE_IPV4_HNAPT;
		break;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		offload_type = EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T;
		break;
	default:
		ppe->diag[EN75_DIAG_REPL_REJ_BASIC]++;
		return -EOPNOTSUPP;
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				en75_flow_offload_mangle_eth(act, &data.eth);
			break;
		case FLOW_ACTION_REDIRECT:
			odev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (data.vlan.num + data.pppoe.num == 2 ||
			    act->vlan.proto != htons(ETH_P_8021Q)) {
				ppe->diag[EN75_DIAG_REPL_REJ_ACT]++;
				return -EOPNOTSUPP;
			}

			data.vlan.vlans[data.vlan.num].id = act->vlan.vid;
			data.vlan.vlans[data.vlan.num].proto = act->vlan.proto;
			data.vlan.num++;
			break;
		case FLOW_ACTION_VLAN_POP:
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (data.pppoe.num == 1 || data.vlan.num == 2) {
				ppe->diag[EN75_DIAG_REPL_REJ_ACT]++;
				return -EOPNOTSUPP;
			}

			data.pppoe.sid = act->pppoe.sid;
			data.pppoe.num++;
			break;
		default:
			ppe->diag[EN75_DIAG_REPL_REJ_ACT]++;
			return -EOPNOTSUPP;
		}
	}

	/* The FOE entry can only egress via GDM1, i.e. out of the wired
	 * MT7530 ports. A flow whose output device is anything else - a
	 * Wi-Fi netdev, or the ingress side of an unresolved forward path
	 * (XMIT_NEIGH redirects to the other direction's iif) - must stay
	 * on the software path: binding it would switch the packets out
	 * the wired ports and black-hole the flow.
	 */
	if (!odev || odev != ppe->ndev) {
		ppe->diag[EN75_DIAG_REPL_REJ_ODEV]++;
		return -EOPNOTSUPP;
	}

	if (!is_valid_ether_addr(data.eth.h_source) ||
	    !is_valid_ether_addr(data.eth.h_dest)) {
		ppe->diag[EN75_DIAG_REPL_REJ_MAC]++;
		return -EINVAL;
	}

	/* Egress to the GDM port that feeds the external MT7530 over TRGMII.
	 * The L2 rewrite (incl. the VLAN push from the flow actions) tells the
	 * switch which physical port to use, so a single GDM serves LAN+WAN.
	 * The bound flow is now forwarded entirely in hardware (no CPU). */
	err = en75_foe_entry_prepare(&foe, offload_type, l4proto,
				     EN75_PPE_PSE_GDM1_PORT,
				     data.eth.h_source, data.eth.h_dest);
	if (err)
		return err;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports ports;

		flow_rule_match_ports(rule, &ports);
		data.src_port = ports.key->src;
		data.dst_port = ports.key->dst;
	} else {
		ppe->diag[EN75_DIAG_REPL_REJ_PORTS]++;
		return -EOPNOTSUPP;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs addrs;

		flow_rule_match_ipv4_addrs(rule, &addrs);
		data.v4.src_addr = addrs.key->src;
		data.v4.dst_addr = addrs.key->dst;

		en75_flow_set_ipv4_addr(&foe, &data, false);
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs addrs;

		flow_rule_match_ipv6_addrs(rule, &addrs);
		data.v6.src_addr = addrs.key->src;
		data.v6.dst_addr = addrs.key->dst;

		en75_foe_entry_set_ipv6_tuple(&foe,
					      data.v6.src_addr.s6_addr32,
					      data.src_port,
					      data.v6.dst_addr.s6_addr32,
					      data.dst_port);
	}

	/* Snapshot the ingress tuple now: the second action pass rewrites
	 * data.*_port / data.v4 in place with the egress (post-NAT) values.
	 * This key is what the RX-hint commit matches incoming packets on.
	 */
	en75_flow_tuple_from_data(&keytuple, addr_type, l4proto, &data);

	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_MANGLE)
			continue;

		switch (act->mangle.htype) {
		case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
		case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
			err = en75_flow_mangle_ports(act, &data);
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
			err = en75_flow_mangle_ipv4(act, &data);
			break;
		case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
			/* handled earlier */
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (err)
			return err;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		err = en75_flow_set_ipv4_addr(&foe, &data, true);
		if (err)
			return err;
	}

	for (i = 0; i < data.vlan.num; i++)
		en75_foe_entry_set_vlan(&foe, data.vlan.vlans[i].id);

	if (data.pppoe.num == 1)
		en75_foe_entry_set_pppoe(&foe, data.pppoe.sid);

	/* Stamp the bind timestamp so the aging scan does not cull the entry
	 * the moment it is committed.
	 */
	foe.ib1 &= ~EN75_FOE_IB1_BIND_TIMESTAMP;
	foe.ib1 |= en75_ppe_timestamp(ppe);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->cookie = f->cookie;
	entry->tuple = keytuple;
	entry->foe = foe;
	entry->hash = ~0u;
	entry->state = EN75_FLOW_PENDING;
	/* Let the first RX bind hint scan immediately. kzalloc leaves last_scan
	 * = 0, but jiffies starts at INITIAL_JIFFIES (-300*HZ), so a zero reads
	 * as "scanned ~300s in the future" and the backoff check suppresses every
	 * commit for the first ~5 min of uptime (observed: backoff=412815,
	 * bound=0 at 3 min uptime). Backdate by 1s (> the HZ/10 backoff) so the
	 * first attempt always proceeds regardless of the jiffies sign. */
	entry->last_scan = jiffies - HZ;

	/* Stage only: the entry is published into the live table later, by
	 * en75_ppe_offload_rx_commit(), once the hardware tells us which slot
	 * it searches for this tuple. Until then the flow stays on the CPU
	 * path exactly as before.
	 */
	err = rhashtable_insert_fast(&ppe->flow_table, &entry->node,
				     en75_flow_ht_params);
	if (err < 0) {
		kfree(entry);
		return err;
	}

	err = rhashtable_insert_fast(&ppe->tuple_table, &entry->tuple_node,
				     en75_tuple_ht_params);
	if (err < 0) {
		rhashtable_remove_fast(&ppe->flow_table, &entry->node,
				       en75_flow_ht_params);
		kfree(entry);
		return err;
	}

	ppe->diag[keytuple.is_v6 ? EN75_DIAG_REPL_OK6 : EN75_DIAG_REPL_OK4]++;

	return 0;
}

static int
en75_ppe_offload_destroy(struct en75_ppe *ppe, struct flow_cls_offload *f)
{
	struct en75_flow_entry *entry;
	unsigned long flags;

	entry = rhashtable_lookup(&ppe->flow_table, &f->cookie,
				  en75_flow_ht_params);
	if (!entry)
		return -ENOENT;

	rhashtable_remove_fast(&ppe->tuple_table, &entry->tuple_node,
			       en75_tuple_ht_params);
	rhashtable_remove_fast(&ppe->flow_table, &entry->node,
			       en75_flow_ht_params);

	/* Serialise the slot teardown and the DEAD transition against a
	 * concurrent RX-hint commit of the same entry.
	 */
	spin_lock_irqsave(&ppe->foe_lock, flags);
	if (entry->state == EN75_FLOW_COMMITTED)
		__en75_ppe_foe_clear(ppe, entry->hash);
	entry->state = EN75_FLOW_DEAD;
	spin_unlock_irqrestore(&ppe->foe_lock, flags);

	kfree_rcu(entry, rcu);

	return 0;
}

static int
en75_ppe_offload_stats(struct en75_ppe *ppe, struct flow_cls_offload *f)
{
	struct en75_flow_entry *entry;

	entry = rhashtable_lookup(&ppe->flow_table, &f->cookie,
				  en75_flow_ht_params);
	if (!entry)
		return -ENOENT;

	/* Hardware accounting is not wired up yet; report the flow as freshly
	 * used so the software flowtable does not reap it. The flow is still
	 * torn down via FLOW_CLS_DESTROY when conntrack expires.
	 */
	f->stats.lastused = jiffies;

	return 0;
}

/*
 * Find the slot where the engine auto-learned (FORWARD_BUILD) an UNBIND entry
 * for this flow. The descriptor-reported hash is not the engine's real search
 * slot on this SoC, so committing there never hits; the engine keeps its own
 * UNBIND entry at the true hash slot and asks us to bind it (crsn 0x0e/0x0f).
 * Scan for that entry by matching the ingress tuple, and bind it in place.
 * @want is host byte order; the live table is little-endian.
 */
/* True if the UNBIND entry the engine learned at slot @i matches @want. */
/* Min jiffies between engine-slot scans for one still-unbound flow. */
#define EN75_PPE_SCAN_BACKOFF (HZ / 10)

static bool en75_ppe_slot_matches(struct en75_ppe *ppe, u32 i,
				  const struct en75_foe_entry *want)
{
	u32 type = FIELD_GET(EN75_FOE_IB1_PACKET_TYPE, want->ib1);
	u32 ib1;

	if (i >= EN75_PPE_ENTRIES)
		return false;

	ib1 = le32_to_cpu(READ_ONCE(ppe->foe[i].ib1));
	if (FIELD_GET(EN75_FOE_IB1_STATE, ib1) != EN75_FOE_STATE_UNBIND)
		return false;
	if (FIELD_GET(EN75_FOE_IB1_PACKET_TYPE, ib1) != type)
		return false;

	if (type == EN75_PPE_PKT_TYPE_IPV4_HNAPT ||
	    type == EN75_PPE_PKT_TYPE_IPV4_ROUTE) {
		const struct en75_ipv4_tuple *w = &want->ipv4.orig;
		const struct en75_ipv4_tuple *h = &ppe->foe[i].ipv4.orig;

		return le32_to_cpu(h->src_ip)  == w->src_ip &&
		       le32_to_cpu(h->dest_ip) == w->dest_ip &&
		       le32_to_cpu(h->ports)   == w->ports;
	}
	if (type == EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T ||
	    type == EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T) {
		const struct en75_foe_ipv6 *w = &want->ipv6;
		const struct en75_foe_ipv6 *h = &ppe->foe[i].ipv6;
		int j;

		for (j = 0; j < 4; j++)
			if (le32_to_cpu(h->src_ip[j])  != w->src_ip[j] ||
			    le32_to_cpu(h->dest_ip[j]) != w->dest_ip[j])
				return false;
		if (type == EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T &&
		    le32_to_cpu(h->ports) != w->ports)
			return false;
		return true;
	}
	return false;
}

/*
 * The slot the engine actually searches. The RX descriptor's ppe_entry field
 * reports the matching FOE entry scaled by 80/32 = 2.5 (the engine counts in
 * 32-byte units while an entry is 80 bytes), so the field holds real_slot*5/2.
 * Verified on hardware: hw_slot 40->16, 6840->2736, 4440->1776, 4510->1804.
 *
 * But ppe_entry is only 14 bits (max 16383) while real_slot*5/2 reaches 40957
 * for the upper slots, so the field is TRUNCATED mod 16384. Every slot this
 * O(1) path is asked to produce is a *primary* hash slot (hash<<1, see
 * EN75_PPE_HASH_OFFSET in econet_foe.h: two FOE entries share a bucket, and
 * only the even bucket-base address is ever computed by a hash function -
 * the odd "second way" of a colliding pair is the engine's own choice, never
 * ours), so real_slot is always even here and real_slot*5/2 = 5*hash. The
 * truncation then drops whole multiples of 16384: field = 5*hash - m*16384
 * with the wrap count m in {0,1,2} (5*hash <= 40955 < 3*16384). m is
 * recoverable directly: since -16384 == 1 mod 5, field mod 5 == (5*hash -
 * m*16384) mod 5 == m. Undo the wrap before scaling:
 *   real_slot = (hw_slot + (hw_slot % 5) * 16384) * 2 / 5
 * This makes the upper ~60% of the table O(1) too (previously they missed the
 * scaled guess and fell through to the full scan, pinning the CPU in softirq).
 * If the engine ever reports the odd "second way" slot of a bucket instead
 * (a real 2-way collision), this formula is not guaranteed to invert it
 * correctly - the full scan below remains the safety net for exactly that
 * case, and for any other genuinely unexpected descriptor.
 */
/*
 * The engine's FOE hash for IPv4, verified on hardware against the engine's
 * own learned UNBIND slots (30/30 exact). It returns the descriptor-scaled
 * value (real_slot * 5/2), so the live table index is that * 2 / 5. Inputs are
 * the host-order orig tuple, exactly as staged in entry->foe:
 *   s1 = ports, s3 = dest_ip, s6 = src_ip
 *   s7 = (~s1 & s6) | (s3 & s1)
 *   s2 = (s1 ^ s3 ^ s6) ^ ror32(s7, 24)
 *   fold = (s2 & 0x1fff) ^ ((s2 >> 13) & 0x1fff) ^ (s2 >> 26)
 *   slot = ((fold & 0x1fff) << 1) * 2 / 5
 * This places the BIND at the exact slot the engine searches, with no
 * descriptor hint and no table scan, so it works for every flow (not just the
 * ~40% the 14-bit descriptor field could address) and beats the engine's own
 * FORWARD_BUILD auto-bind to the slot.
 */
static u32 en75_ppe_ipv4_hash_slot(const struct en75_foe_entry *want)
{
	u32 s1 = want->ipv4.orig.ports;
	u32 s3 = want->ipv4.orig.dest_ip;
	u32 s6 = want->ipv4.orig.src_ip;
	u32 s7 = ((~s1) & s6) | (s3 & s1);
	u32 s2 = (s1 ^ s3 ^ s6) ^ ((s7 >> 24) | (s7 << 8));	/* ^ ror32(s7,24) */
	u32 fold = ((s2 & 0x1fff) ^ ((s2 >> 13) & 0x1fff) ^ (s2 >> 26)) & 0x1fff;

	return ((fold << 1) * 2) / 5;
}

static int en75_ppe_find_engine_slot(struct en75_ppe *ppe,
				     const struct en75_foe_entry *want,
				     u32 hw_slot)
{
	u32 type = FIELD_GET(EN75_FOE_IB1_PACKET_TYPE, want->ib1);
	u32 i, cand;

	cand = (hw_slot + (hw_slot % 5) * EN75_PPE_ENTRIES) * 2 / 5;
	if (en75_ppe_slot_matches(ppe, cand, want))
		return cand;

	for (i = 0; i < EN75_PPE_ENTRIES; i++)
		if (en75_ppe_slot_matches(ppe, i, want))
			return i;

	/* If FORWARD_BUILD has not learned this flow yet, IPv4 can still use
	 * the recovered vendor hash as a fallback. In the common 0x0e/0x0f
	 * path above, however, the learned entry wins so collisions/way choice
	 * stay exactly as the engine selected them.
	 */
	if (type == EN75_PPE_PKT_TYPE_IPV4_HNAPT ||
	    type == EN75_PPE_PKT_TYPE_IPV4_ROUTE) {
		i = en75_ppe_ipv4_hash_slot(want);
		return (i < EN75_PPE_ENTRIES) ? (int)i : -1;
	}

	/* IPv6 has no equivalent recovered hash; the descriptor-scaled
	 * candidate and the full scan above already covered every type,
	 * so there is nothing left to try.
	 */
	return -1;
}

void en75_ppe_offload_rx_commit(struct en75_ppe *ppe, struct sk_buff *skb,
				u32 hw_slot)
{
	struct en75_flow_entry *entry;
	struct en75_flow_tuple key;
	unsigned long flags;

	if (IS_ERR_OR_NULL(ppe) || !ppe->tuple_table_ready)
		return;
	if (hw_slot >= EN75_PPE_ENTRIES)
		return;

	ppe->diag[EN75_DIAG_RXC_CALL]++;

	en75_flow_tuple_from_skb(&key, skb);
	if (!key.l4proto) {
		ppe->diag[EN75_DIAG_RXC_NOPARSE]++;
		return;
	}

	rcu_read_lock();
	entry = rhashtable_lookup(&ppe->tuple_table, &key,
				  en75_tuple_ht_params);
	if (!entry) {
		ppe->diag[key.is_v6 ? EN75_DIAG_RXC_NOLK6 :
				      EN75_DIAG_RXC_NOLK4]++;
	} else if (READ_ONCE(entry->state) != EN75_FLOW_PENDING) {
		ppe->diag[EN75_DIAG_RXC_NOTPEND]++;
	} else if (time_before(jiffies,
			       READ_ONCE(entry->last_scan) + EN75_PPE_SCAN_BACKOFF)) {
		ppe->diag[EN75_DIAG_RXC_BACKOFF]++;
	} else {
		int slot;

		WRITE_ONCE(entry->last_scan, jiffies);

		/* Resolve the slot the engine searches for this flow (O(1) from
		 * the descriptor; full scan only as a fallback). The commit below
		 * re-checks the state under the lock, so a racy read only risks a
		 * retry. */
		slot = en75_ppe_find_engine_slot(ppe, &entry->foe, hw_slot);
		if (slot < 0) {
			ppe->diag[key.is_v6 ? EN75_DIAG_RXC_NOSLOT6 :
					      EN75_DIAG_RXC_NOSLOT4]++;
		} else {
			spin_lock_irqsave(&ppe->foe_lock, flags);
			if (entry->state == EN75_FLOW_PENDING) {
				struct en75_foe_entry e = entry->foe;

				e.ib1 &= ~(EN75_FOE_IB1_STATE | EN75_FOE_IB1_BIND_TIMESTAMP);
				e.ib1 |= FIELD_PREP(EN75_FOE_IB1_STATE, EN75_FOE_STATE_BIND);
				e.ib1 |= en75_ppe_timestamp(ppe);
				__en75_ppe_foe_commit(ppe, slot, &e);
				entry->hash = slot;
				entry->state = EN75_FLOW_COMMITTED;
				ppe->diag[key.is_v6 ? EN75_DIAG_RXC_BOUND6 :
						      EN75_DIAG_RXC_BOUND4]++;
			}
			spin_unlock_irqrestore(&ppe->foe_lock, flags);
		}
	}
	rcu_read_unlock();
}

static DEFINE_MUTEX(en75_flow_offload_mutex);

int en75_ppe_offload_cmd(struct en75_ppe *ppe, struct flow_cls_offload *cls)
{
	int err;

	if (IS_ERR_OR_NULL(ppe) || !ppe->flow_table_ready)
		return -EOPNOTSUPP;

	mutex_lock(&en75_flow_offload_mutex);
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		err = en75_ppe_offload_replace(ppe, cls);
		break;
	case FLOW_CLS_DESTROY:
		err = en75_ppe_offload_destroy(ppe, cls);
		break;
	case FLOW_CLS_STATS:
		err = en75_ppe_offload_stats(ppe, cls);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&en75_flow_offload_mutex);

	return err;
}

/*
 * Indirect flow blocks. Without DSA the kernel flowtable offers a flow on the
 * higher-layer device it traverses (a bridge or VLAN), which has no
 * ndo_setup_tc of its own, so the offload is delivered through the indirect
 * block mechanism instead of the port's direct ndo_setup_tc. We accept every
 * such device: a flow that does not actually traverse our hardware simply
 * never matches a packet and ages out, and en75_ppe_offload_replace() already
 * rejects anything whose resolved egress is not GDM1.
 */
struct en75_indr_priv {
	struct list_head list;
	struct net_device *dev;
	struct en75_ppe *ppe;
};

static LIST_HEAD(en75_indr_block_cb_list);

static int en75_setup_indr_block_cb(enum tc_setup_type type, void *type_data,
				    void *cb_priv)
{
	struct en75_indr_priv *priv = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return en75_ppe_offload_cmd(priv->ppe, type_data);
}

static struct en75_indr_priv *
en75_indr_block_cb_lookup(struct en75_ppe *ppe, struct net_device *dev)
{
	struct en75_indr_priv *priv;

	list_for_each_entry(priv, &ppe->indr_block_list, list)
		if (priv->dev == dev)
			return priv;

	return NULL;
}

static void en75_setup_indr_rel(void *cb_priv)
{
	struct en75_indr_priv *priv = cb_priv;

	list_del(&priv->list);
	kfree(priv);
}

static int en75_setup_indr_block(struct net_device *dev, struct Qdisc *sch,
				 struct en75_ppe *ppe,
				 struct flow_block_offload *f, void *data,
				 void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct en75_indr_priv *cb_priv;
	struct flow_block_cb *block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		cb_priv = kmalloc(sizeof(*cb_priv), GFP_KERNEL);
		if (!cb_priv)
			return -ENOMEM;

		cb_priv->dev = dev;
		cb_priv->ppe = ppe;
		list_add(&cb_priv->list, &ppe->indr_block_list);

		block_cb = flow_indr_block_cb_alloc(en75_setup_indr_block_cb,
						    cb_priv, cb_priv,
						    en75_setup_indr_rel, f,
						    dev, sch, data, ppe,
						    cleanup);
		if (IS_ERR(block_cb)) {
			list_del(&cb_priv->list);
			kfree(cb_priv);
			return PTR_ERR(block_cb);
		}

		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &en75_indr_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		cb_priv = en75_indr_block_cb_lookup(ppe, dev);
		if (!cb_priv)
			return -ENOENT;

		block_cb = flow_block_cb_lookup(f->block,
						en75_setup_indr_block_cb,
						cb_priv);
		if (!block_cb)
			return -ENOENT;

		flow_indr_block_cb_remove(block_cb, f);
		list_del(&block_cb->driver_list);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int en75_setup_indr_cb(struct net_device *dev, struct Qdisc *sch,
			      void *cb_priv, enum tc_setup_type type,
			      void *type_data, void *data,
			      void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct en75_ppe *ppe = cb_priv;

	if (!dev)
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return en75_setup_indr_block(dev, sch, ppe, type_data, data,
					     cleanup);
	default:
		return -EOPNOTSUPP;
	}
}

static const char * const en75_diag_names[EN75_DIAG_COUNT] = {
	[EN75_DIAG_RXC_CALL]	  = "rxc_call",
	[EN75_DIAG_RXC_NOPARSE]	  = "rxc_noparse",
	[EN75_DIAG_RXC_NOLK4]	  = "rxc_nolookup_v4",
	[EN75_DIAG_RXC_NOLK6]	  = "rxc_nolookup_v6",
	[EN75_DIAG_RXC_NOTPEND]	  = "rxc_notpending",
	[EN75_DIAG_RXC_BACKOFF]	  = "rxc_backoff",
	[EN75_DIAG_RXC_NOSLOT4]	  = "rxc_noslot_v4",
	[EN75_DIAG_RXC_NOSLOT6]	  = "rxc_noslot_v6",
	[EN75_DIAG_RXC_BOUND4]	  = "rxc_bound_v4",
	[EN75_DIAG_RXC_BOUND6]	  = "rxc_bound_v6",
	[EN75_DIAG_REPL_OK4]	  = "repl_ok_v4",
	[EN75_DIAG_REPL_OK6]	  = "repl_ok_v6",
	[EN75_DIAG_REPL_REJ_BASIC] = "repl_rej_basic",
	[EN75_DIAG_REPL_REJ_ACT]  = "repl_rej_action",
	[EN75_DIAG_REPL_REJ_ODEV] = "repl_rej_odev",
	[EN75_DIAG_REPL_REJ_MAC]  = "repl_rej_mac",
	[EN75_DIAG_REPL_REJ_PORTS] = "repl_rej_ports",
};

static int en75_diag_show(struct seq_file *m, void *v)
{
	struct en75_ppe *ppe = m->private;
	int i;

	for (i = 0; i < EN75_DIAG_COUNT; i++)
		seq_printf(m, "%-16s %u\n", en75_diag_names[i], ppe->diag[i]);
	for (i = 0; i < 32; i++)
		if (ppe->diag_crsn[i])
			seq_printf(m, "crsn_%02x          %u\n", i,
				   ppe->diag_crsn[i]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en75_diag);

int en75_ppe_offload_init(struct en75_ppe *ppe)
{
	int err;

	INIT_LIST_HEAD(&ppe->indr_block_list);

	err = rhashtable_init(&ppe->flow_table, &en75_flow_ht_params);
	if (err)
		return err;

	err = rhashtable_init(&ppe->tuple_table, &en75_tuple_ht_params);
	if (err) {
		rhashtable_destroy(&ppe->flow_table);
		return err;
	}

	err = flow_indr_dev_register(en75_setup_indr_cb, ppe);
	if (err) {
		rhashtable_destroy(&ppe->tuple_table);
		rhashtable_destroy(&ppe->flow_table);
		return err;
	}

	ppe->flow_table_ready = true;
	ppe->tuple_table_ready = true;

	debugfs_create_file("diag", 0444, ppe->debugfs, ppe,
			    &en75_diag_fops);

	return 0;
}

static void en75_flow_free_cb(void *ptr, void *arg)
{
	kfree(ptr);
}

void en75_ppe_offload_deinit(struct en75_ppe *ppe)
{
	if (!ppe->flow_table_ready)
		return;

	ppe->flow_table_ready = false;
	ppe->tuple_table_ready = false;
	flow_indr_dev_unregister(en75_setup_indr_cb, ppe, en75_setup_indr_rel);
	/* Wait out any RX-hint lookup that already passed the ready check. */
	synchronize_rcu();
	/* Drop the tuple index first so no RX commit can find an entry while
	 * the cookie table is being torn down and the entries freed.
	 */
	rhashtable_destroy(&ppe->tuple_table);
	rhashtable_free_and_destroy(&ppe->flow_table, en75_flow_free_cb, NULL);
}
