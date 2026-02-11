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

static u8* mtk_tag_prepare(struct sk_buff *skb, struct dsa_port *dp, int push_tags)
{
	u8 xmit_tpid = MTK_HDR_XMIT_UNTAGGED;
	struct dsa_switch *ds = dp->ds;
	struct dsa_switch *cpu_ds;
	u8 *mtk_tag = NULL;

	if (WARN_ON_ONCE(!dp->cpu_dp))
		return NULL;

	cpu_ds = dp->cpu_dp->ds;

	if (cpu_ds == ds) {
		/* We're done, we can start allocating tags */
		int space = push_tags;

		/* If we're VLAN'd last tag replaces VLAN hdr
		 * otherwise we must allocate that too */
		switch (skb->protocol) {
		case htons(ETH_P_8021Q):
			xmit_tpid = MTK_HDR_XMIT_TAGGED_TPID_8100;
			break;
		case htons(ETH_P_8021AD):
			xmit_tpid = MTK_HDR_XMIT_TAGGED_TPID_88A8;
			break;
		default:
			space++;
		}

		if (space) {
			skb_push(skb, MTK_HDR_LEN * space);
			dsa_alloc_etype_header(skb, MTK_HDR_LEN * space);
		}

		mtk_tag = dsa_etype_header_pos_tx(skb);
	} else {
		/* We have more switches to traverse. */
		struct dsa_switch_tree *dst = ds->dst;
		struct dsa_link *dl;
	
		list_for_each_entry(dl, &dst->rtable, list) {
			if (dl->dp->ds != ds || dl->link_dp->ds != cpu_ds)
				continue;

			mtk_tag = mtk_tag_prepare(skb, dl->link_dp, push_tags + 1);
			break;
		}
	}

	if (WARN_ON_ONCE(!mtk_tag))
		return NULL;

	/* Mark tag attribute on special tag insertion to notify hardware
	 * whether that's a combined special tag with 802.1Q header.
	 */
	mtk_tag[0] = xmit_tpid;
	mtk_tag[1] = (1 << dp->index) & MTK_HDR_XMIT_DP_BIT_MASK;

	/* Tag control information is kept for 802.1Q */
	if (xmit_tpid == MTK_HDR_XMIT_UNTAGGED) {
		mtk_tag[2] = 0;
		mtk_tag[3] = 0;
	}

	return &mtk_tag[4];
}

static struct sk_buff *mtk_tag_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);

	skb_set_queue_mapping(skb, dp->index);

	/* The Ethernet switch we are interfaced with needs packets to be at
	 * least 64 bytes (including FCS) otherwise their padding might be
	 * corrupted. With tags enabled, we need to make sure that packets are
	 * at least 68 bytes (including FCS and tag).
	 */
	eth_skb_pad(skb);

	if (!mtk_tag_prepare(skb, dp, 0))
		return NULL;

	return skb;
}

static struct sk_buff *mtk_pull_port(struct sk_buff *skb, int *port)
{
	__be16 *phdr;
	u16 hdr;

	if (unlikely(!pskb_may_pull(skb, MTK_HDR_LEN)))
		return NULL;

	phdr = dsa_etype_header_pos_rx(skb);
	hdr = ntohs(*phdr);

	/* Remove MTK tag and recalculate checksum. */
	skb_pull_rcsum(skb, MTK_HDR_LEN);

	dsa_strip_etype_header(skb, MTK_HDR_LEN);

	/* Get source port information */
	*port = (hdr & MTK_HDR_RECV_SOURCE_PORT_MASK);

	return skb;
}

static struct sk_buff *mtk_recv(struct sk_buff *skb, struct dsa_port *upstream)
{
	struct dsa_switch_tree *dst = upstream->ds->dst;
	int device = upstream->ds->index;
	struct dsa_port *dp;
	struct dsa_link *dl;
	int port;

	skb = mtk_pull_port(skb, &port);
	if (!skb)
		return NULL;

	list_for_each_entry(dp, &dst->ports, list) {

		/* Not the port we're looking for */
		if (dp->ds->index != device || dp->index != port)
			continue;

		/* Found the port, we're done */
		if (dp->type == DSA_PORT_TYPE_USER) {
			skb->dev = dp->user;

			if (!skb->dev)
				return NULL;

			dsa_default_offload_fwd_mark(skb);

			return skb;
		}

		/* If it's not a link between switches, it's wrong */
		if (dp->type != DSA_PORT_TYPE_DSA)
			return NULL;

		/* Find the other side of the link */
		list_for_each_entry(dl, &dst->rtable, list) {
			if (dl->dp != dp)
				continue;

			/* Go to the next header with the new switch */
			return mtk_recv(skb, dl->link_dp);
		}

		/* Didn't find an other side */
		return NULL;
	}

	/* Didn't find a port with that number */
	return NULL;
}

static struct sk_buff *mtk_tag_rcv(struct sk_buff *skb, struct net_device *dev)
{
	return mtk_recv(skb, dev->dsa_ptr);
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
