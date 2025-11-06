// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#ifndef MTK_ETH_H
#define MTK_ETH_H

#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/u64_stats_sync.h>
#include <linux/refcount.h>
#include <linux/phylink.h>

#include "econet_eth.h"

#define	MTK_MAX_RX_LENGTH	1536
#define MTK_MAC_COUNT		2
#define MTK_RX_ETH_HLEN		(VLAN_ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN)
#define MTK_DEFAULT_MSG_ENABLE	(NETIF_MSG_DRV | \
				 NETIF_MSG_PROBE | \
				 NETIF_MSG_LINK | \
				 NETIF_MSG_TIMER | \
				 NETIF_MSG_IFDOWN | \
				 NETIF_MSG_IFUP | \
				 NETIF_MSG_RX_ERR | \
				 NETIF_MSG_TX_ERR)

/* GMA1 Received Good Byte Count Register */
#define MTK_GDM1_TX_GBCNT	0x2400
#define MTK_STAT_OFFSET		0x40


/* PHY Indirect Access Control registers */
// #define MTK_PHY_IAC		0x10004
#define PHY_IAC_ACCESS		BIT(31)
#define PHY_IAC_READ		BIT(19)
#define PHY_IAC_WRITE		BIT(18)
#define PHY_IAC_START		BIT(16)
#define PHY_IAC_ADDR_SHIFT	20
#define PHY_IAC_REG_SHIFT	25
#define PHY_IAC_TIMEOUT		HZ

enum mtk_dev_state {
	MTK_HW_INIT,
	MTK_RESETTING
};

/* currently no SoC has more than 2 macs */
#define MTK_MAX_DEVS			2

struct mtk_mac;

struct mtk_eth {
	struct device			*dev;
	void __iomem			*base;
	spinlock_t			page_lock;
	spinlock_t			tx_irq_lock;
	spinlock_t			rx_irq_lock;
	struct net_device		*netdev[MTK_MAX_DEVS];
	struct mtk_mac			*mac[MTK_MAX_DEVS];
	int				irq[3];
	u32				msg_enable;
	refcount_t			dma_refcnt;

	struct mii_bus			*mii_bus;
	struct work_struct		pending_work;
	unsigned long			state;

	struct en75_debug		*debug;
	// struct qdma			qdma[NUM_QDMA];
};

struct mtk_mac {
	int				id;
	struct device_node		*of_node;
	struct mtk_eth			*hw;
};

#endif /* MTK_ETH_H */
