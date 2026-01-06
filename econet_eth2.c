// SPDX-License-Identifier: GPL-2.0-only
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/of_net.h>

#include "gdm_regs.h"
#include "linux/dev_printk.h"
#include "linux/ioport.h"
#include "linux/mdio.h"
#include "qdma_desc.h"
#include "qdma_regs.h"
#include "econet_eth.h"
#include "econet_port.h"

#define AIROHA_MAX_NUM_GDM_PORTS	2



struct en75_eth {
	struct device *dev;
	struct net_device *ports[AIROHA_MAX_NUM_GDM_PORTS];
	// struct airoha_gdm_port *ports[AIROHA_MAX_NUM_GDM_PORTS];
	struct en751221_regs __iomem *regs;
	struct reset_control *reset;
	int qdma_irq[NUM_QDMA * EN75_QDMA_IRQS];
	struct en75_qdma *qdma[NUM_QDMA];
};

union gdm_regs {
	struct gdm regs;
	u32 words[0x800 / sizeof(u32)];
};

struct en751221_regs {
	u32 fe[0x400 / sizeof(u32)];			/* BFB50000 - BFB50400 */
	union gdm_regs port0;				/* BFB50400 - BFB50C00 */
	u32 ppe[0x400 / sizeof(u32)];			/* BFB50C00 - BFB51000 */
	u32 unknown_deadbeef[0x400 / sizeof(u32)];	/* BFB51000 - BFB51400 */
	union gdm_regs port1;				/* BFB51400 - BFB51C00 */
	u32 unknown_deadbeef2[0x400 / sizeof(u32)];	/* BFB51C00 - BFB52000 */
	u32 ppe_accounting[0x400 / sizeof(u32)];	/* BFB52000 - BFB52400 */
	u32 ppe_unused[0x1c00 / sizeof(u32)];		/* BFB52400 - BFB54000 */
	union {
		struct qregs qdma_regs;
		u32 qdma0[0x1000 / sizeof(u32)];
	} qdma[NUM_QDMA]; 				/* BFB54000 - BFB56000 */
	u32 unknown_zeroed[0x2000 / sizeof(u32)];	/* BFB56000 - BFB58000 */
	u32 switch_regs[0x8000 / sizeof(u32)];		/* BFB58000 - BFB60000 */
};
_Static_assert(sizeof(struct en751221_regs) == 0x10000, "en751221_regs size incorrect");


static struct net_device *en75_get_sport_dev(struct en75_eth *eth, int sport)
{
	if (sport == ETX_FPORT_WAN) {
		return eth->ports[1];
	} else {
		if (sport != ETX_FPORT_LAN)
			dev_info(eth->dev, "rx: on unexpected fport %d\n", sport);
		return eth->ports[0];
	}
}

int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb, int sport)
{
	struct net_device *port;

	port = en75_get_sport_dev(eth, sport);

	skb->dev = port;
	skb->protocol = eth_type_trans(skb, port);

	if (netdev_uses_dsa(port)) {
		/* PPE module requires untagged packets to work
		 * properly and it provides DSA port index via the
		 * DMA descriptor. Report DSA tag to the DSA stack
		 * via skb dst info.
		 */

		// On EN751221 generally, this is not done, but the
		// EN7526C is special cased and it does do this for
		// that one. If we need it, we'll want to do one
		// codepath for everything. For now we'll do nothing
		// and hope that the tag-basd DSA works.
		//port_num = (sp_tag & 0x7); /*switch port id*/
		#if 0
		u32 sptag = desc.t.erx.sp_tag;
		if (sptag < ARRAY_SIZE(port->dsa_meta) &&
			port->dsa_meta[sptag])
			skb_dst_set_noref(q->skb,
						&port->dsa_meta[sptag]->dst);
		#endif
	}

	return 0;
}



// static const struct net_device_ops airoha_netdev_ops = {
// 	.ndo_init		= airoha_dev_init,
// 	.ndo_open		= airoha_dev_open,
// 	.ndo_stop		= airoha_dev_stop,
// 	.ndo_change_mtu		= airoha_dev_change_mtu,
// 	.ndo_select_queue	= airoha_dev_select_queue,
// 	.ndo_start_xmit		= airoha_dev_xmit,
// 	.ndo_get_stats64        = airoha_dev_get_stats64,
// 	.ndo_set_mac_address	= airoha_dev_set_macaddr,
// 	// .ndo_setup_tc		= airoha_dev_tc_setup,
// };

// static const struct ethtool_ops airoha_ethtool_ops = {
// 	.get_drvinfo		= airoha_ethtool_get_drvinfo,
// 	.get_eth_mac_stats      = airoha_ethtool_get_mac_stats,
// 	.get_rmon_stats		= airoha_ethtool_get_rmon_stats,
// };


static int en75_init_port(struct en75_eth *eth, struct device_node *np)
{
	const __be32 *id_ptr = of_get_property(np, "reg", NULL);
	struct net_device *dev;
	u32 id;

	if (!id_ptr) {
		dev_err(eth->dev, "missing gdm port id\n");
		return -EINVAL;
	}

	id = be32_to_cpup(id_ptr);

	if (!id || id > ARRAY_SIZE(eth->ports)) {
		dev_err(eth->dev, "invalid gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (eth->ports[id - 1]) {
		dev_err(eth->dev, "duplicate gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (id == 0)
		dev = en75_alloc_gdm_port(eth->dev, np,
					  &eth->regs->port0.regs,
					  eth->qdma[0],
					  ETX_FPORT_LAN,
					  false);
	else if (id == 1)
		dev = en75_alloc_gdm_port(eth->dev, np,
					  &eth->regs->port0.regs,
					  eth->qdma[1],
					  ETX_FPORT_WAN,
					  true);
	else
		return -EINVAL;

	if (IS_ERR(dev))
		return PTR_ERR(dev);
	
	eth->ports[id - 1] = dev;
	return 0;
}

// void en75_eth_new(struct platform_device *pdev)
// {
// 	// Get the registers. One register means everything is in one block.
// 	// If 4 registers then they are: [FE, QDMA0, QDMA1, SWITCH]
// 	//
// 	// Get the IRQs, the soc tells us how many there should be
// 	// Create the number of QDMA instances per the SoC
// 	static int airoha_qdma_new(struct platform_device *pdev,
// 			   void __iomem *qdma_regs,
// 			   int id,
// 			   int *irqs,
// 			   int num_irqs,
// 			   struct en75_qdma_cfg *cfg,
// 			   struct en75_eth *eth)
// }


static void en75_prepare_qdma_cfg(struct en75_qdma_cfg *cfg)
{
	// Configure QDMA parameters
	memset(cfg, 0, sizeof(*cfg));
	for (int i = 0; i < AIROHA_NUM_RX_RING; i++)
		cfg->num_rx_descs[i] = 8;
	for (int i = 0; i < AIROHA_NUM_TX_RING; i++)
		cfg->num_tx_descs[i] = 8;
	for (int i = 0; i < AIROHA_NUM_TX_DONE; i++)
		cfg->done_list_size[i] = 32;
	cfg->fwd_max_packet_size = 2048;
	cfg->fwd_low_threshold = 4;
	cfg->num_fwd_descs = 8;
}

static void airoha_remove(struct platform_device *pdev)
{
	struct en75_eth *eth = platform_get_drvdata(pdev);
	int i;

	if (!eth)
		return;

	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++) {
		if (!eth->qdma[i])
			continue;

		en75_qdma_destroy(eth->qdma[i]);
	}

	for (i = 0; i < ARRAY_SIZE(eth->ports); i++) {
		if (!eth->ports[i])
			continue;

		// airoha_dev_stop(port->dev);
		unregister_netdev(eth->ports[i]);
		// airoha_metadata_dst_free(port);
	}
	// free_netdev(eth->napi_dev);

	// airoha_ppe_deinit(eth);
	platform_set_drvdata(pdev, NULL);
}

static int airoha_probe(struct platform_device *pdev)
{
	struct resource *regs_res;
	struct en75_qdma_cfg cfg;
	struct device_node *np;
	struct en75_eth *eth;
	struct en751221_regs __iomem *regs;
	int i, err, irq;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->dev = &pdev->dev;
	platform_set_drvdata(pdev, eth);

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return dev_err_probe(&pdev->dev, err, 
				     "failed setting DMA mask\n");

	regs = devm_platform_get_and_ioremap_resource(pdev, 0, &regs_res);
	if (IS_ERR(regs))
		return dev_err_probe(&pdev->dev, PTR_ERR(regs),
				     "failed to map registers\n");

	if (resource_size(regs_res) < sizeof(struct en751221_regs)) {
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "insufficient register space\n");
	}

	eth->regs = regs;

	eth->reset = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(eth->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(eth->reset),
				     "failed to get resets\n");

	for (i = 0; i < ARRAY_SIZE(eth->qdma_irq); i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
					     "failed to get IRQ %d\n", i);
		eth->qdma_irq[i] = irq;
	}

	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma) != ARRAY_SIZE(regs->qdma));
	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma) != ARRAY_SIZE(eth->qdma_irq) * EN75_QDMA_IRQS);
	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++) {
		en75_prepare_qdma_cfg(&cfg);
		eth->qdma[i] = en75_qdma_new(pdev, &regs->qdma[i], i,
					     &eth->qdma_irq[i * EN75_QDMA_IRQS],
					     EN75_QDMA_IRQS, &cfg, eth);

		if (IS_ERR(eth->qdma[i])) {
			err = PTR_ERR(eth->qdma[i]);
			eth->qdma[i] = NULL;
			goto error;
		}
	}

	for_each_child_of_node(pdev->dev.of_node, np) {
		if (!of_device_is_compatible(np, "econet,eth-mac"))
			continue;

		if (!of_device_is_available(np))
			continue;

		err = en75_init_port(eth, np);
		if (err) {
			of_node_put(np);
			goto error;
		}
	}

error:
	airoha_remove(pdev);
	return err;
}

static const struct of_device_id of_airoha_match[] = {
	{ .compatible = "econet,en751221-eth" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_airoha_match);

static struct platform_driver airoha_driver = {
	.probe = airoha_probe,
	.remove = airoha_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_airoha_match,
	},
};
module_platform_driver(airoha_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Caleb James DeLisle <cjd@cjdns.fr>");
MODULE_DESCRIPTION("Ethernet driver for EcoNet SoC");