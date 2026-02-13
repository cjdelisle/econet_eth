// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek DSA Tag support
 * Copyright (C) 2017 Landen Chao <landen.chao@mediatek.com>
 *		      Sean Wang <sean.wang@mediatek.com>
 */

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>

#include "tag.h"

#define MTK_NAME		"mtk"

#define MTK_HDR_LEN		4
#define MTK_HDR_XMIT_UNTAGGED		0
#define MTK_HDR_XMIT_TAGGED_TPID_8100	1
#define MTK_HDR_XMIT_TAGGED_TPID_88A8	2
#define MTK_HDR_RECV_SOURCE_PORT_MASK	GENMASK(2, 0)
#define MTK_HDR_XMIT_DP_BIT_MASK	GENMASK(5, 0)
#define MTK_HDR_XMIT_SA_DIS		BIT(6)
#define MTK_HDR_XMIT_PASSTHROUGH	BIT(7)

static struct sk_buff *mtk_tag_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u8 xmit_tpid;
	u8 *mtk_tag;

	skb_set_queue_mapping(skb, dp->index);

	/* The Ethernet switch we are interfaced with needs packets to be at
	 * least 64 bytes (including FCS) otherwise their padding might be
	 * corrupted. With tags enabled, we need to make sure that packets are
	 * at least 68 bytes (including FCS and tag).
	 */
	eth_skb_pad(skb);

	/* Build the special tag after the MAC Source Address. If VLAN header
	 * is present, it's required that VLAN header and special tag is
	 * being combined. Only in this way we can allow the switch can parse
	 * the both special and VLAN tag at the same time and then look up VLAN
	 * table with VID.
	 */
	switch (skb->protocol) {
	case htons(ETH_P_8021Q):
		xmit_tpid = MTK_HDR_XMIT_TAGGED_TPID_8100;
		break;
	case htons(ETH_P_8021AD):
		xmit_tpid = MTK_HDR_XMIT_TAGGED_TPID_88A8;
		break;
	default:
		xmit_tpid = MTK_HDR_XMIT_UNTAGGED;
		skb_push(skb, MTK_HDR_LEN);
		dsa_alloc_etype_header(skb, MTK_HDR_LEN);
	}

	mtk_tag = dsa_etype_header_pos_tx(skb);

	/* Mark tag attribute on special tag insertion to notify hardware
	 * whether that's a combined special tag with 802.1Q header.
	 */
	mtk_tag[0] = xmit_tpid;
	mtk_tag[1] = (1 << dp->index) & MTK_HDR_XMIT_DP_BIT_MASK;

	/* If not switch 0, we must set the passthrough bit */
	if (dp->ds->index)
		mtk_tag[1] |= MTK_HDR_XMIT_PASSTHROUGH;

	/* Tag control information is kept for 802.1Q */
	if (xmit_tpid == MTK_HDR_XMIT_UNTAGGED) {
		mtk_tag[2] = 0;
		mtk_tag[3] = 0;
	}

	return skb;
}

/* Implementation of dsa_conduit_find_user() which does not care about which
 * switch device the user port is found on.
 *
 * MT7530 switches support chaining, but with only one downstream switch, and
 * the downstream and upstream switches cannot share any port numbers. If port
 * 4 is enabled on the downstream switch, it cannot also be enabled on the
 * upstream. Therefore we can scan for a matching port without regard for
 * which switch it is on.
 */
static inline struct net_device *mtk_conduit_find_user(struct net_device *dev,
						       int port)
{
	struct dsa_port *cpu_dp = dev->dsa_ptr;
	struct dsa_switch_tree *dst = cpu_dp->dst;
	struct dsa_port *dp;

	list_for_each_entry(dp, &dst->ports, list)
		if (dp->index == port &&
		    dp->type == DSA_PORT_TYPE_USER)
			return dp->user;

	return NULL;
}


static struct sk_buff *mtk_tag_rcv(struct sk_buff *skb, struct net_device *dev)
{
	u16 hdr;
	int port;
	__be16 *phdr;

	if (unlikely(!pskb_may_pull(skb, MTK_HDR_LEN)))
		return NULL;

	phdr = dsa_etype_header_pos_rx(skb);
	hdr = ntohs(*phdr);

	/* Remove MTK tag and recalculate checksum. */
	skb_pull_rcsum(skb, MTK_HDR_LEN);

	dsa_strip_etype_header(skb, MTK_HDR_LEN);

	/* Get source port information */
	port = (hdr & MTK_HDR_RECV_SOURCE_PORT_MASK);

	skb->dev = mtk_conduit_find_user(dev, port);
	if (!skb->dev)
		return NULL;

	dsa_default_offload_fwd_mark(skb);

	return skb;
}

static const struct dsa_device_ops mtk_netdev_ops = {
	.name		= MTK_NAME,
	.proto		= DSA_TAG_PROTO_MTK,
	.xmit		= mtk_tag_xmit,
	.rcv		= mtk_tag_rcv,
	.needed_headroom = MTK_HDR_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for Mediatek switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_MTK, MTK_NAME);

module_dsa_tag_driver(mtk_netdev_ops);
