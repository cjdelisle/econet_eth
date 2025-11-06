// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#include <linux/of_device.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/if_vlan.h>
#include <linux/reset.h>
#include <linux/tcp.h>
#include <linux/interrupt.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/platform_device.h>

///

#include "econet_eth1.h"
#include "qdma_desc.h"

#define UNUSED __attribute__((unused))


///
#define GDMA1_BASE			0x0500
#define GDMA1_MAC_ADRL			(GDMA1_BASE + 0x08)
#define GDMA1_MAC_ADRH			(GDMA1_BASE + 0x0c)
#define GSW_BASE			0x8000
#define GSW_MAC_BASE			(GSW_BASE + 0x3000)
#define GSW_SMACCR0			(GSW_MAC_BASE + 0xe4)
#define GSW_SMACCR1			(GSW_MAC_BASE + 0xe8)

#define MTK_QDMA_INT_STATUS		0x4050
#define MTK_QDMA_INT_MASK		0x4054
// MTK_QDMA_INT_MASK bits.
#define INT_STATUS_HWFWD_DSCP_LOW	BIT(10)
#define INT_STATUS_IRQ_FULL		BIT(9)
#define INT_STATUS_HWFWD_DSCP_EMPTY	BIT(8)
#define INT_STATUS_NO_RX0_CPU_DSCP      BIT(3)
#define INT_STATUS_NO_TX0_CPU_DSCP	BIT(2)
#define INT_STATUS_RX0_DONE		BIT(1)
#define INT_STATUS_TX0_DONE		BIT(0)

#define QDMA_CSR_HWFWD_DSCP_BASE	0x4020
#define QDMA_CSR_HWFWD_BUFF_BASE	0x4024
#define QDMA_CSR_HWFWD_DSCP_CFG		0x4028
#define QDMA_CSR_LMGR_INIT_CFG		0x4030

#define QDMA_CSR_LMGR_START_BIT		BIT(31)


#define QDMA_CSR_IRQ_STATUS		0x406C
#define QDMA_CSR_IRQ_CLEAR_LEN		0x4068
#define IRQ_STATUS_HEAD_IDX_MASK	0xFFF
#define IRQ_STATUS_ENTRY_LEN_SHIFT	16
#define IRQ_STATUS_ENTRY_LEN_MASK	(0xFFF << IRQ_STATUS_ENTRY_LEN_SHIFT)
#define IRQ_DEF_VALUE			0xFFFFFFFF


#define QDMA_CSR_IRQ_BASE		0x4060
#define QDMA_CSR_IRQ_CFG		0x4064
#define QDMA_IRQ_QUEUE_DEPTH		20


static int mtk_msg_level = -1;
module_param_named(msg_level, mtk_msg_level, int, 0);
MODULE_PARM_DESC(msg_level, "Message level (-1=defaults,0=none,...,16=all)");

static void mtk_w32(struct mtk_eth *eth, u32 val, unsigned reg)
{
	__raw_writel(val, eth->base + reg);
}

static u32 mtk_r32(struct mtk_eth *eth, unsigned reg)
{
	return __raw_readl(eth->base + reg);
}

#define MTK_PHY_IAC		0xf01c

static int mtk_mdio_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;

	while (1) {
		if (!(mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_ACCESS))
			return 0;
		if (time_after(jiffies, t_start + PHY_IAC_TIMEOUT))
			break;
		usleep_range(10, 20);
	}

	dev_err(eth->dev, "mdio: MDIO timeout\n");
	return -1;
}

static u32 _mtk_mdio_write(struct mtk_eth *eth, u32 phy_addr,
			   u32 phy_register, u32 write_data)
{
	if (mtk_mdio_busy_wait(eth))
		return -1;

	write_data &= 0xffff;

	mtk_w32(eth, PHY_IAC_ACCESS | PHY_IAC_START | PHY_IAC_WRITE |
		(phy_register << PHY_IAC_REG_SHIFT) |
		(phy_addr << PHY_IAC_ADDR_SHIFT) | write_data,
		MTK_PHY_IAC);

	if (mtk_mdio_busy_wait(eth))
		return -1;

	return 0;
}

static u32 _mtk_mdio_read(struct mtk_eth *eth, int phy_addr, int phy_reg)
{
	u32 d;

	if (mtk_mdio_busy_wait(eth))
		return 0xffff;

	mtk_w32(eth, PHY_IAC_ACCESS | PHY_IAC_START | PHY_IAC_READ |
		(phy_reg << PHY_IAC_REG_SHIFT) |
		(phy_addr << PHY_IAC_ADDR_SHIFT),
		MTK_PHY_IAC);

	if (mtk_mdio_busy_wait(eth))
		return 0xffff;

	d = mtk_r32(eth, MTK_PHY_IAC) & 0xffff;

	return d;
}

static int mtk_mdio_write(struct mii_bus *bus, int phy_addr,
			  int phy_reg, u16 val)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_write(eth, phy_addr, phy_reg, val);
}

static int mtk_mdio_read(struct mii_bus *bus, int phy_addr, int phy_reg)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_read(eth, phy_addr, phy_reg);
}

static int UNUSED mtk_mdio_init(struct mtk_eth *eth)
{
	struct device_node *mii_np;
	int ret;

	mii_np = of_get_child_by_name(eth->dev->of_node, "mdio-bus");
	if (!mii_np) {
		dev_err(eth->dev, "no %s child node found", "mdio-bus");
		return -ENODEV;
	}

	if (!of_device_is_available(mii_np)) {
		ret = -ENODEV;
		goto err_put_node;
	}

	eth->mii_bus = devm_mdiobus_alloc(eth->dev);
	if (!eth->mii_bus) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	eth->mii_bus->name = "mdio";
	eth->mii_bus->read = mtk_mdio_read;
	eth->mii_bus->write = mtk_mdio_write;
	eth->mii_bus->priv = eth;
	eth->mii_bus->parent = eth->dev;

	snprintf(eth->mii_bus->id, MII_BUS_ID_SIZE, "%pOFn", mii_np);
	ret = of_mdiobus_register(eth->mii_bus, mii_np);

err_put_node:
	of_node_put(mii_np);
	return ret;
}

static void mtk_mdio_cleanup(struct mtk_eth *eth)
{
	if (!eth->mii_bus)
		return;

	mdiobus_unregister(eth->mii_bus);
}

static int en75_set_mac_address(struct net_device *dev, void *p)
{
	int ret = eth_mac_addr(dev, p);
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const char *macaddr = dev->dev_addr;
	
	if (ret)
		return ret;

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	spin_lock_bh(&mac->hw->page_lock);

	mtk_w32(eth,
		macaddr[2]<<24 | macaddr[3]<<16 |
		macaddr[4]<<8  | macaddr[5]<<0,
		GDMA1_MAC_ADRL);
	mtk_w32(eth,
		macaddr[0]<<8  | macaddr[1]<<0,
		GDMA1_MAC_ADRH);

	/* fill in switch's MAC address */
	mtk_w32(eth,
		macaddr[2]<<24 | macaddr[3]<<16 |
                macaddr[4]<<8  | macaddr[5]<<0, GSW_SMACCR0);
	mtk_w32(eth,
		macaddr[0]<<8  | macaddr[1]<<0, GSW_SMACCR1);

	spin_unlock_bh(&mac->hw->page_lock);

	return 0;
}

#define QDMA_HWFWD_DESC_SIZE	16

#define TX0_DSCP_NUM	4
#define RX0_DSCP_NUM	4
#define DSCP_NUM	(TX0_DSCP_NUM + RX0_DSCP_NUM)
#define HWFWD_DSCP_NUM	8

static void *hw_fwd_ary = NULL;
static void *hw_fwd_buff = NULL;

static struct qdma_desc *dscp_ary = NULL;
static struct sk_buff *dscp_sk_buff_p_ary[DSCP_NUM];

static u32 *irq_queue = NULL;

#define QDMA_CSR_TX_DSCP_BASE	0x4008
#define QDMA_CSR_RX_DSCP_BASE	0x400C
#define QDMA_CSR_RX_RING_CFG	0x4100
#define QDMA_CSR_RX_RING_THR	0x4104

#define QDMA_CSR_TX_CPU_IDX	0x4010
#define QDMA_CSR_TX_DMA_IDX	0x4014

#define QDMA_CSR_RX_CPU_IDX	0x4018
#define QDMA_CSR_RX_DMA_IDX	0x401C

#define QDMA_CSR_GLB_CFG	0x4004

/* #define DEBUG 1 */
/* #define TX_DEBUG 1 */
/* #define RX_DEBUG 1 */

static void tx0_free_skb(struct mtk_eth *eth, int idx, struct qdma_desc *dscp)
{
	struct sk_buff *skb;
	
	skb = dscp_sk_buff_p_ary[idx];
	dscp_sk_buff_p_ary[idx] = NULL;
	dma_map_single(eth->dev,
		       (void *)dscp->pkt_addr, skb_headlen(skb), DMA_FROM_DEVICE);
	dev_kfree_skb(skb);	
}

static void tx0_free_some(struct mtk_eth *eth, struct qdma_desc *dscp)
{
	int i, idx;
	
	for (i = 0; i < 2; i++) {
		idx = get_desc_next_idx(dscp);
		dscp = &dscp_ary[idx];
		if (dscp->pkt_addr) {
			tx0_free_skb(eth, idx, dscp);
			dscp->pkt_addr = 0;
		}
		/* TODO: If done = 0, adjust drop counter. */
		
		/* Set done = 0. */
		set_desc_done(dscp, 0);
		// memset(&dscp->ctrl, 0, sizeof(uint));
	}
}

static int tx0_dscp_pkt_addr(struct mtk_eth *eth,
			     struct sk_buff *skb, int idx)
{
	dma_addr_t phys;
	
	phys = dma_map_single(eth->dev,
			      skb->data, skb_headlen(skb), DMA_TO_DEVICE);
	if (!phys) {
		return 0;
	}
	dscp_sk_buff_p_ary[idx] = skb;
	
	return phys;
}

static struct qdma_desc *tx0_get_dscp(int idx)
{
	return &dscp_ary[idx];
}	

static int mtk_tx_map(struct sk_buff *skb, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct qdma_desc_etx *tx_msg;
	struct qdma_desc *dscp;
	int idx; //, val;

#ifdef TX_DEBUG
	print_dscp_ary(eth);
	print_hw_fwd_ary(eth);
#endif
	
	idx = mtk_r32(eth, QDMA_CSR_TX_CPU_IDX) % TX0_DSCP_NUM;
#ifdef TX_DEBUG
	printk("(1) CPU idx %d, DMA idx %d.",
	       idx, mtk_r32(eth, QDMA_CSR_TX_DMA_IDX));
#endif		
	dscp = tx0_get_dscp(idx);

	tx_msg = &dscp->t.etx;
	set_etx_fport(tx_msg, 1); /* GDM_P_GDMA1 */

	if (skb->len < 60) {
		skb_padto(skb, 60);
		skb_put(skb, 60 - skb->len);
	}
	
	dscp->pkt_addr = tx0_dscp_pkt_addr(eth, skb, idx);
	if (! dscp->pkt_addr) {
		/* free skb. */
		return -1;
	}
	dscp->pkt_len = skb_headlen(skb);
	// dscp->ctrl.done = 0;

	/* QDMA_CSR_DMA_IDX will move to an element with
	   done = 0. If element is not found, `done` marking will stop.

	   Will become very busy, GLB_CFG_TX_DMA_BUSY, if it will not find 
	   a packet for sending. */

	/* TODO: We already freed some dscp's, no need to call this
	   everytime.
	   if (idx % 2) tx0_free_some(dscp); */
	tx0_free_some(eth, dscp);
	
	// wmb();
	
	mtk_w32(eth, get_desc_next_idx(dscp), QDMA_CSR_TX_CPU_IDX);

#ifdef TX_DEBIG
	printk("(2) CPU idx %d, DMA idx %d, next_idx %d.",
	       mtk_r32(eth, QDMA_CSR_TX_CPU_IDX),
	       mtk_r32(eth, QDMA_CSR_TX_DMA_IDX),
	       dscp->next_idx);
#endif	
	return 0;
}


static netdev_tx_t mtk_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct net_device_stats *stats = &dev->stats;
	
	/* normally we can rely on the stack not calling this more than once,
	 * however we have 2 queues running on the same ring so we need to lock
	 * the ring access
	 */
	spin_lock(&eth->page_lock);

	if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
		goto drop;


	if (mtk_tx_map(skb, dev) < 0)
		goto drop;

	spin_unlock(&eth->page_lock);

	return NETDEV_TX_OK;

drop:
	spin_unlock(&eth->page_lock);
	stats->tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void mtk_dma_free(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++)
		if (eth->netdev[i])
			netdev_reset_queue(eth->netdev[i]);
}

static void mtk_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	eth->netdev[mac->id]->stats.tx_errors++;
	netif_err(eth, tx_err, dev,
		  "transmit timed out\n");
	schedule_work(&eth->pending_work);
}

static struct qdma_desc *rx0_get_dscp(int idx) {
	return &dscp_ary[TX0_DSCP_NUM + idx];
}

static struct sk_buff *rx0_new_skb(struct mtk_eth *eth,
				   int idx, struct qdma_desc *dscp)
{
	int len;
	struct sk_buff *new_skb;
	dma_addr_t phys;

	len = 2000;

	new_skb = alloc_skb(len, GFP_ATOMIC);
	if (!new_skb) {
		return NULL;
	}

	phys = dma_map_single(eth->dev, new_skb->data, len, DMA_TO_DEVICE);
	if (!phys) {
		// free new_skb.
		return NULL;
	}
	dscp_sk_buff_p_ary[TX0_DSCP_NUM + idx] = new_skb;
	dscp->pkt_addr = phys;
	
	return new_skb;
}

static struct sk_buff *rx0_pop_skb(struct mtk_eth *eth,
				   int idx, struct qdma_desc *dscp)
{
	int len;
	struct sk_buff *skb;
	dma_addr_t phys;

	len = 2000;
	
	skb = dscp_sk_buff_p_ary[idx + TX0_DSCP_NUM];
	phys = dscp->pkt_addr;
	if (rx0_new_skb(eth, idx, dscp)) {
		dma_unmap_single(eth->dev, phys, len, DMA_FROM_DEVICE);
		return skb;
	} 
	return NULL;
}

static void rx0_dscp_defaults(struct qdma_desc *dscp)
{
	/* TODO: May be this is not necessary. The is payload size. */
	dscp->pkt_len = 1518;
}

static void rx0_done(struct mtk_eth *eth)
{
	int idx, val;
	struct qdma_desc *dscp;
	struct sk_buff *skb;
	
	val = mtk_r32(eth, QDMA_CSR_RX_DMA_IDX);
#ifdef RX_DEBUG	
	printk("rx0_done (1) CPU = %d, DMA = %d.",
	       mtk_r32(eth, QDMA_CSR_RX_CPU_IDX), val);
#endif
	/* Get previous ring index. */
	idx = (val + (RX0_DSCP_NUM - 1)) % RX0_DSCP_NUM;

	dscp = rx0_get_dscp(idx);
	skb = rx0_pop_skb(eth, idx, dscp);	
	if (skb) {
		skb_put(skb, dscp->pkt_len);
		/* TODO: Get netdev by switch port. How? */
		skb->protocol = eth_type_trans(skb, eth->netdev[0]);
		netif_rx(skb);
	} else {
		/* TODO: Update netdev drop counter. */
	}
	rx0_dscp_defaults(dscp);
	set_desc_done(dscp, false);
	/* DMA ID will try to meet CPU ID, no need to assign
	   CPU ID on every new message. I do this to free
	   my eyes from if's. */
	mtk_w32(eth, idx, QDMA_CSR_RX_CPU_IDX);
#ifdef RX_DEBUG
	printk("rx0_done (2) CPU = %d, DMA = %d.",
	       mtk_r32(eth, QDMA_CSR_RX_CPU_IDX),
	       mtk_r32(eth, QDMA_CSR_RX_DMA_IDX));
#endif
}

static void tx0_recycle_if_required(struct mtk_eth *eth)
{
	int val, idx, len, i;
	
	/* Irq queue keeps indexes of sent tx dscp's so we know
	   which skb's and dscp's we can free. TX interrupt can 
	   be configured to trigger once in N messages, 
	   QDMA_CSR_TX_DELAY_INT_CFG. 
	   See 7512_eth.c qdma_bm_transmit_done.
	   
	   I free skb's in xmit. It looks like irq queue is not necessary,
	   but this simplified mode did not work. 
	   
	   TODO. */

	/*  The IRQ_FULL interrupt will be triggered if len == QUEUE_DEPTH.

	    Clean the queue counter. 

	    The counter will not be set 0 by writing to CLEAR_LEN reg, it 
	    will continue until len == IRQ_DEPTH and then begin 
	    from 0. */
	
	val = mtk_r32(eth, QDMA_CSR_IRQ_STATUS);
	idx = val & IRQ_STATUS_HEAD_IDX_MASK;
	len = (val & IRQ_STATUS_ENTRY_LEN_MASK) >> IRQ_STATUS_ENTRY_LEN_SHIFT;
	/* printk("IRQ Q STATUS %x, %d, %d.", val, idx, len); */

	for (i = 0; i < len; i++) {
		irq_queue[i] = IRQ_DEF_VALUE;
	}
	mtk_w32(eth, len & 0x7F, QDMA_CSR_IRQ_CLEAR_LEN);
}

static irqreturn_t mtk_handle_irq(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;
	int status, mask;

	mask = mtk_r32(eth, MTK_QDMA_INT_MASK);
	status = mtk_r32(eth, MTK_QDMA_INT_STATUS);
	
	pr_debug("mtk int mask=%x status=%x.", mask, status);

	if (status & INT_STATUS_RX0_DONE) {
		rx0_done(eth);
	} else if (status & INT_STATUS_TX0_DONE) {
		tx0_recycle_if_required(eth);
	}
	
	mtk_w32(eth, status & mask, MTK_QDMA_INT_STATUS);
	
	return IRQ_HANDLED;
}

static void qdma_initialize_hw_fwd(struct mtk_eth *eth) {
	dma_addr_t phys_addr;
	int i, val, len;

	// mtk/linux-2.6.36/*.i, qdma_bm_dscp_init().
	// DSCP "done" marking will not begin if this is not set.
	mtk_w32(eth, 0x14 << 16, QDMA_CSR_LMGR_INIT_CFG);
	
	// Alloc mem for HWFWD_DSCPs.
	len = QDMA_HWFWD_DESC_SIZE * HWFWD_DSCP_NUM;
	hw_fwd_ary = dma_alloc_coherent(eth->dev,
					len, &phys_addr, GFP_ATOMIC);
	memset(hw_fwd_ary, 0, len);
	mtk_w32(eth, phys_addr, QDMA_CSR_HWFWD_DSCP_BASE);

	// Alloc HWFWD buf, depends on payload size.
	hw_fwd_buff = dma_alloc_coherent(eth->dev, 2048, &phys_addr, GFP_ATOMIC);
	memset(hw_fwd_buff, 0, 2048);
	mtk_w32(eth, phys_addr, QDMA_CSR_HWFWD_BUFF_BASE);

	val = mtk_r32(eth, QDMA_CSR_LMGR_INIT_CFG);
	mtk_w32(eth, val | HWFWD_DSCP_NUM, QDMA_CSR_LMGR_INIT_CFG);
	// Payload.
	mtk_w32(eth, 0 << 28, QDMA_CSR_HWFWD_DSCP_CFG);
	// Set threshold.
	mtk_w32(eth, 1, QDMA_CSR_HWFWD_DSCP_CFG);

	// Bootloader register value.
	// mtk_w32(eth, 0x1180004, QDMA_CSR_LMGR_INIT_CFG);

	val = mtk_r32(eth, QDMA_CSR_LMGR_INIT_CFG);
	mtk_w32(eth, val | QDMA_CSR_LMGR_START_BIT, QDMA_CSR_LMGR_INIT_CFG);	
	// Wait for init.
	for (i = 0; i < 100; i++) {
		val = mtk_r32(eth, QDMA_CSR_LMGR_INIT_CFG);
		if ((val & QDMA_CSR_LMGR_START_BIT) == 0)
			break;
	}
	// TODO: report init failure.
}

static void qdma_initialize_irq_queue(struct mtk_eth *eth)
{
	dma_addr_t phys;
	int len;

	len = QDMA_IRQ_QUEUE_DEPTH * sizeof(u32);
	
	irq_queue = dma_alloc_coherent(eth->dev, len, &phys, GFP_ATOMIC);
	memset(irq_queue, IRQ_DEF_VALUE, len);
	mtk_w32(eth, phys, QDMA_CSR_IRQ_BASE);
	mtk_w32(eth, QDMA_IRQ_QUEUE_DEPTH, QDMA_CSR_IRQ_CFG);	
}

static void qdma_initialize_tx_ring(void) {
	int i;
	for (i = 0; i < TX0_DSCP_NUM - 1; i++)
		set_desc_next_idx(&dscp_ary[i], i + 1);
}

static void qdma_initialize_rx_ring(struct mtk_eth *eth) {
	struct qdma_desc *dscp;
	int i;
	
	for (i = 0; i < RX0_DSCP_NUM; i++) {
		dscp = rx0_get_dscp(i);
		rx0_dscp_defaults(dscp);
		rx0_new_skb(eth, i, dscp);
	}	
}

static int qdma_config(struct mtk_eth *eth)
{
	// int err, i, val;	
	dma_addr_t phys_addr;
	
	// Disable TX/RX.
	mtk_w32(eth, 0, QDMA_CSR_GLB_CFG);
	
	dscp_ary = dma_alloc_coherent(eth->dev,
			      sizeof(struct qdma_desc) * DSCP_NUM,
				      &phys_addr,
				      GFP_ATOMIC);
	memset(dscp_ary, 0, sizeof(struct qdma_desc) * DSCP_NUM);

	struct en75_debug_conf debug_conf = {0};
	debug_conf.qdma[0].regs = eth->base + 0x4000;
	// pr_info("REGS=%.8x\n", (u32) debug_conf.qdma[0].regs);
	debug_conf.qdma[0].chains[0].rx_descs = (struct qdma_desc *) rx0_get_dscp(0);
	debug_conf.qdma[0].chains[0].rx_count = RX0_DSCP_NUM;
	debug_conf.qdma[0].chains[0].tx_descs = (struct qdma_desc *) tx0_get_dscp(0);
	debug_conf.qdma[0].chains[0].tx_count = TX0_DSCP_NUM;
	eth->debug = en75_debugfs_init(&debug_conf);

	// Set TX and RX DSCP addresses.
	mtk_w32(eth, phys_addr, QDMA_CSR_TX_DSCP_BASE);
	mtk_w32(eth, phys_addr + sizeof(struct qdma_desc) * TX0_DSCP_NUM, QDMA_CSR_RX_DSCP_BASE);
	
	mtk_w32(eth, DSCP_NUM - TX0_DSCP_NUM, QDMA_CSR_RX_RING_CFG);
	mtk_w32(eth, 0, QDMA_CSR_RX_RING_THR);

	qdma_initialize_irq_queue(eth);
	qdma_initialize_hw_fwd(eth);

	qdma_initialize_tx_ring();	
	// Set TX circular buffer/ring pointers.
	mtk_w32(eth, 0, QDMA_CSR_TX_CPU_IDX);
	mtk_w32(eth, 0, QDMA_CSR_TX_DMA_IDX);

	qdma_initialize_rx_ring(eth);       	
	mtk_w32(eth, 0, QDMA_CSR_RX_CPU_IDX);
	mtk_w32(eth, 0, QDMA_CSR_RX_DMA_IDX);
	mtk_w32(eth, RX0_DSCP_NUM, QDMA_CSR_RX_CPU_IDX);
	
	// QDMA_CSR_TX_DELAY_INT_CFG 
	mtk_w32(eth, 0, 0x4058);
	// RX_DELAY_INT_CFG
	mtk_w32(eth, 0, 0x405C);

	mtk_w32(eth, (1 << 27) | (1 << 26) | (1 << 28) | (0x3 << 4)
		| QCFG_TX_DMA_EN | QCFG_RX_DMA_EN |
		(1 << 6) | (1 << 4) | (1 << 5)
		/* GLB_CFG_RX_2B_OFFSET. 7512_eth.c  _receive_buffer. */
		/* | (1 << 31) */
		/* GLB_CFG_IRQ_EN */
	        | (1 << 19),
		QDMA_CSR_GLB_CFG);

	/* Select interrupts.
	   If INT_STATUS_TX0_DONE is off but GLB_CFG_IRQ_EN is on, 
	   TX0_DONE interrupt will be triggered. 
	   If INT_STATUS_TX0_DONE and GLB_CFG_IRQ_EN both on, TX0_DONE
	   will be triggered even if no message was received. */	
	mtk_w32(eth, INT_STATUS_HWFWD_DSCP_LOW |
		INT_STATUS_IRQ_FULL |
		INT_STATUS_HWFWD_DSCP_EMPTY |
		INT_STATUS_NO_RX0_CPU_DSCP |
		INT_STATUS_NO_TX0_CPU_DSCP |
		INT_STATUS_RX0_DONE /* | INT_STATUS_TX0_DONE */,
		MTK_QDMA_INT_MASK);
	
	// GDMA1_FWD_CFG from bootloader mem.
	mtk_w32(eth, 0xC0000000, 0x500);

	// GSW_PMCR from bootloader reg.
	mtk_w32(eth, 0x9E30B, 0x8000 + 0x3000 + 5 * 0x100);
	mtk_w32(eth, 0x9E30B, 0x8000 + 0x3000 + 6 * 0x100);
	
	// GSW_MFC, matches bootloader reg value.
	mtk_w32(eth, (0xff << 24) | (0xff << 16) | (0xff << 8) | (1 << 7) | (6 << 4), 0x8000 + 0x10);
	
	return 0;
}

static int mtk_open(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	// int err;

	// err = phylink_of_phy_connect(mac->phylink, mac->of_node, 0);
	// if (err) {
	// 	netdev_err(dev, "%s: could not attach PHY: %d\n", __func__,
	// 		   err);
	// 	return err;
	// }

	/* we run 2 netdevs on the same dma ring so we only bring it up once */
	if (!refcount_read(&eth->dma_refcnt)) {
		int err = qdma_config(eth);

		if (err)
			return err;

		// mtk_gdm_config(eth, MTK_GDMA_TO_PDMA);

		// mtk_tx_irq_enable(eth, MTK_TX_DONE_INT); -- this was not me
		// mtk_rx_irq_enable(eth, MTK_RX_DONE_INT);
		refcount_set(&eth->dma_refcnt, 1);
	}
	else
		refcount_inc(&eth->dma_refcnt);

	// phylink_start(mac->phylink);
	netif_start_queue(dev);
	return 0;
}

static void mtk_stop_dma(struct mtk_eth *eth, u32 glo_cfg)
{
	u32 val;
	int i;

	/* stop the dma engine */
	spin_lock_bh(&eth->page_lock);
	val = mtk_r32(eth, glo_cfg);
	mtk_w32(eth, val & ~(QCFG_TX_WB_DONE | QCFG_RX_DMA_EN | QCFG_TX_DMA_EN),
		glo_cfg);
	spin_unlock_bh(&eth->page_lock);

	/* wait for dma stop */
	for (i = 0; i < 10; i++) {
		val = mtk_r32(eth, glo_cfg);
		if (val & (QCFG_TX_DMA_BUSY | QCFG_RX_DMA_BUSY)) {
			msleep(20);
			continue;
		}
		break;
	}
}

static int mtk_stop(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	en75_debugfs_exit(eth->debug);
	eth->debug = NULL;

	// phylink_stop(mac->phylink);

	netif_tx_disable(dev);

	// phylink_disconnect_phy(mac->phylink);

	/* only shutdown DMA if this is the last user */
	if (!refcount_dec_and_test(&eth->dma_refcnt))
		return 0;

	// mtk_gdm_config(eth, MTK_GDMA_DROP_ALL);

	// mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
	// mtk_rx_irq_disable(eth, MTK_RX_DONE_INT);
	mtk_w32(eth, 0, MTK_QDMA_INT_MASK);

	mtk_stop_dma(eth, QDMA_CSR_GLB_CFG);

	mtk_dma_free(eth);

	return 0;
}

static int mtk_hw_deinit(struct mtk_eth *eth)
{
	if (!test_and_clear_bit(MTK_HW_INIT, &eth->state))
		return 0;

	pm_runtime_put_sync(eth->dev);
	pm_runtime_disable(eth->dev);

	return 0;
}

static int __init mtk_init(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const char mac_addr[6] = {16, 163, 184, 106, 1, 8};

	// mac_addr = of_get_mac_address(mac->of_node);
	// if (!IS_ERR(mac_addr))
	dev_addr_set(dev, mac_addr);
	// ether_addr_copy((u8 *)dev->dev_addr, mac_addr);

	/* If the mac address is invalid, use random mac address  */
	if (!is_valid_ether_addr(dev->dev_addr)) {
		eth_hw_addr_random(dev);
		dev_err(eth->dev, "generated random MAC address %pM\n",
			dev->dev_addr);
	}

	return 0;
}

static void mtk_uninit(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	// phylink_disconnect_phy(mac->phylink);
	// mtk_tx_irq_disable(eth, ~0);
	// mtk_rx_irq_disable(eth, ~0);
	mtk_w32(eth, 0, MTK_QDMA_INT_MASK);
}

static int mtk_free_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		free_netdev(eth->netdev[i]);
	}

	return 0;
}

static int mtk_unreg_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		unregister_netdev(eth->netdev[i]);
	}

	return 0;
}

static int mtk_cleanup(struct mtk_eth *eth)
{
	mtk_unreg_dev(eth);
	mtk_free_dev(eth);
	cancel_work_sync(&eth->pending_work);

	return 0;
}

static const struct net_device_ops mtk_netdev_ops = {
	.ndo_init		= mtk_init,
	.ndo_uninit		= mtk_uninit,
	.ndo_open		= mtk_open,
	.ndo_stop		= mtk_stop,
	.ndo_start_xmit		= mtk_start_xmit,
	.ndo_set_mac_address	= en75_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout		= mtk_tx_timeout,
};

static int mtk_add_mac(struct mtk_eth *eth, struct device_node *np)
{
	const __be32 *_id = of_get_property(np, "reg", NULL);
	struct mtk_mac *mac;
	int id;

	if (!_id) {
		dev_err(eth->dev, "missing mac id\n");
		return -EINVAL;
	}

	id = be32_to_cpup(_id);
	if (id >= MTK_MAC_COUNT) {
		dev_err(eth->dev, "%d is not a valid mac id\n", id);
		return -EINVAL;
	}

	if (eth->netdev[id]) {
		dev_err(eth->dev, "duplicate mac id found: %d\n", id);
		return -EINVAL;
	}

	eth->netdev[id] = alloc_etherdev(sizeof(*mac));
	if (!eth->netdev[id]) {
		dev_err(eth->dev, "alloc_etherdev failed\n");
		return -ENOMEM;
	}
	mac = netdev_priv(eth->netdev[id]);
	eth->mac[id] = mac;
	mac->id = id;
	mac->hw = eth;
	mac->of_node = np;

	SET_NETDEV_DEV(eth->netdev[id], eth->dev);
	eth->netdev[id]->watchdog_timeo = 5 * HZ;
	eth->netdev[id]->netdev_ops = &mtk_netdev_ops;
	eth->netdev[id]->base_addr = (unsigned long)eth->base;

	eth->netdev[id]->irq = eth->irq[0];
	eth->netdev[id]->dev.of_node = np;

	eth->netdev[id]->max_mtu = MTK_MAX_RX_LENGTH - MTK_RX_ETH_HLEN;

	return 0;
}

static int mtk_probe(struct platform_device *pdev)
{
	struct device_node *mac_np;
	struct mtk_eth *eth;
	int err, i;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->dev = &pdev->dev;
	eth->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->base))
		return PTR_ERR(eth->base);

	spin_lock_init(&eth->page_lock);
	spin_lock_init(&eth->tx_irq_lock);
	spin_lock_init(&eth->rx_irq_lock);

	for (i = 0; i < 3; i++) {
		if (i > 0)
			eth->irq[i] = eth->irq[0];
		else
			eth->irq[i] = platform_get_irq(pdev, i);
		if (eth->irq[i] < 0) {
			dev_err(&pdev->dev, "no IRQ%d resource found\n", i);
			return -ENXIO;
		}
	}

	eth->msg_enable = netif_msg_init(mtk_msg_level, MTK_DEFAULT_MSG_ENABLE);

	for_each_child_of_node(pdev->dev.of_node, mac_np) {
		if (!of_device_is_compatible(mac_np,
					     "econet,eth-mac"))
			continue;

		if (!of_device_is_available(mac_np))
			continue;

		err = mtk_add_mac(eth, mac_np);
		if (err) {
			of_node_put(mac_np);
			goto err_deinit_hw;
		}
	}

	err = devm_request_irq(eth->dev, eth->irq[0],
			       mtk_handle_irq, 0,
			       dev_name(eth->dev), eth);
		
	if (err)
		goto err_free_dev;

#if 0
	/* No MT7628/88 support yet */
	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		err = mtk_mdio_init(eth);
		if (err)
			goto err_free_dev;
	}
#endif
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		err = register_netdev(eth->netdev[i]);
		if (err) {
			dev_err(eth->dev, "error bringing up device\n");
			goto err_deinit_mdio;
		} else
			netif_info(eth, probe, eth->netdev[i],
				   "EcoNet frame engine at 0x%08lx, irq %d\n",
				   eth->netdev[i]->base_addr, eth->irq[0]);
	}

	platform_set_drvdata(pdev, eth);

	return 0;

err_deinit_mdio:
	mtk_mdio_cleanup(eth);
err_free_dev:
	mtk_free_dev(eth);
err_deinit_hw:
	mtk_hw_deinit(eth);

	return err;
}

static void mtk_remove(struct platform_device *pdev)
{
	struct mtk_eth *eth = platform_get_drvdata(pdev);
	struct mtk_mac *mac;
	int i;

	/* stop all devices to make sure that dma is properly shut down */
	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		mtk_stop(eth->netdev[i]);
		mac = netdev_priv(eth->netdev[i]);
		// phylink_disconnect_phy(mac->phylink);
	}

	mtk_hw_deinit(eth);

	mtk_cleanup(eth);
	mtk_mdio_cleanup(eth);
}

static const struct of_device_id of_mtk_match[] = {
	{ .compatible = "econet,en751221-eth" },
	{},
};
MODULE_DEVICE_TABLE(of, of_mtk_match);

static struct platform_driver mtk_driver = {
	.probe = mtk_probe,
	.remove = mtk_remove,
	.driver = {
		.name = "econet_eth",
		.of_match_table = of_mtk_match,
	},
};

module_platform_driver(mtk_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Caleb James DeLisle <cjd@cjdns.fr>");
MODULE_DESCRIPTION("Ethernet driver for EcoNet EN751221");