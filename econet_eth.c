// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN751221 Ethernet Driver
 *
 * This Ethernet system consists of 2 QDMA engines and 2 GDM ports.
 *
 * The QDMA engine manages DMA and also QoS prioritization, taking advantage of
 * the fact that the packet is in memory and can therefore be sent at the
 * appropriate time.
 *
 * The GDM port is the outward-facing port of the Ethernet device and it
 * corrisponds to the net_device that are registered with the kernel.
 *
 * The QDMAs are *loosely* coupled with the GDM ports, it is possible to send
 * a packet to GDM2 by registering it with QDMA1 or vice versa. On the receive
 * side, the GDM is configured which QDMA to send various types of packets to.
 * Practically speaking, it is sensible to link GDM1 with QDMA1 and GDM2 with
 * QDMA2.
 *
 * Since QDMA engines are responsible for receiving traffic, the QDMA calls
 * en75_rx_before_recv() before calling napi_gro_receive(), this gives the main
 * driver an opportunity to assign the skb a net_device based on which port is
 * indicated in the DMA packet descriptor. Therefore the QDMA driver
 * (econet_qdma.c) has no awareness of the GDM driver (econet_port.c).
 *
 * When sending, the GDM driver must in fact register the packet with a QDMA
 * engine. The main driver (this file) passes an opaque QDMA driver struct to
 * the GDM driver which then calls en75_qdma_xmit() when it has a packet to
 * send. In order to avoid race conditions during driver teardown, the GDM
 * calls en75_qdma_use() and only calls en75_qdma_unuse() when the GDM driver
 * itself is tearing down. The QDMA will not teardown until it no longer has
 * any users. Any number of GDMs could share a QDMA without problem.
 */
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/of_net.h>
#include <linux/iopoll.h>
#include <linux/workqueue.h>

#include "gdm_regs.h"
#include "linux/dev_printk.h"
#include "linux/ioport.h"
#include "linux/mdio.h"
#include "linux/netdevice.h"
#include "qdma_desc.h"
#include "qdma_regs.h"
#include "econet_eth.h"

/* MT7530 switch configuration */
#define  PMCR_FORCE_FDX			BIT(1)
#define  PMCR_FORCE_LNK			BIT(0)
#define  PMCR_FORCE_SPEED_1000		BIT(3)
#define  PMCR_BACKOFF_EN		BIT(9)
#define  PMCR_BACKPR_EN			BIT(8)
#define  PMCR_MAC_MODE			BIT(16)
#define  MT7530_FORCE_MODE		BIT(15)
#define  PMCR_MAC_TX_EN			BIT(14)
#define  PMCR_MAC_RX_EN			BIT(13)
#define  PMCR_IFG_XMIT_MASK		GENMASK(19, 18)
#define  PMCR_IFG_XMIT(x)		FIELD_PREP(PMCR_IFG_XMIT_MASK, x)
#define    IFG_64BIT 2
/* Match bootloader value, 0x9e30b */
#define EN75_PMCR_CONFIG (u32)( \
		PMCR_FORCE_FDX | PMCR_FORCE_LNK | PMCR_FORCE_SPEED_1000 | \
		PMCR_BACKOFF_EN | PMCR_BACKPR_EN | PMCR_MAC_MODE | \
		MT7530_FORCE_MODE | PMCR_MAC_TX_EN | PMCR_MAC_RX_EN | \
		PMCR_IFG_XMIT(IFG_64BIT) \
	)
#define MT753X_PMCR_P(x)		(0x3000 + ((x) * 0x100))

#define MT753X_MFC			0x10
#define  BC_FFP_MASK			GENMASK(31, 24)
#define  BC_FFP(x)			FIELD_PREP(BC_FFP_MASK, x)
#define  UNM_FFP_MASK			GENMASK(23, 16)
#define  UNM_FFP(x)			FIELD_PREP(UNM_FFP_MASK, x)
#define  UNU_FFP_MASK			GENMASK(15, 8)
#define  UNU_FFP(x)			FIELD_PREP(UNU_FFP_MASK, x)
#define  MT7530_CPU_EN			BIT(7)
#define  MT7530_CPU_PORT_MASK		GENMASK(6, 4)
#define  MT7530_CPU_PORT(x)		FIELD_PREP(MT7530_CPU_PORT_MASK, x)

#define EN75_MFC_CONFIG (u32)( \
		BC_FFP(0xff) | UNM_FFP(0xff) | UNU_FFP(0xff) | \
		MT7530_CPU_EN | MT7530_CPU_PORT(6) \
	)

#define SWITCH_MAC_HI			0x30e8
#define SWITCH_MAC_LO			0x30e4

#define EN75_PHY_IAC			0x701c
#define  PHY_IAC_ACCESS			BIT(31)
#define  PHY_IAC_REG(x)		(((x) & 0x1f) << 25)
#define  PHY_IAC_ADDR(x)		(((x) & 0x1f) << 20)
#define  PHY_IAC_READ			BIT(19)
#define  PHY_IAC_WRITE			BIT(18)
#define  PHY_IAC_START			BIT(16)
#define  PHY_IAC_DATA_MASK		GENMASK(15, 0)

#define EN75_MT7530_MDIO_ADDR		0x1f
#define EN75_MT7530_NUM_PORTS		7
#define EN75_MT7530_WAN_UNTAGGED_VID	2

#define MT7530_VTCR			0x90
#define  VTCR_BUSY			BIT(31)
#define  VTCR_FUNC(x)			(((x) & 0xf) << 12)
#define MT7530_VAWD1			0x94
#define  IVL_MAC			BIT(30)
#define  VTAG_EN			BIT(28)
#define  PORT_MEM(x)			(((x) & 0xff) << 16)
#define  FID(x)				(((x) & 0x7) << 1)
#define  VLAN_VALID			BIT(0)
#define MT7530_VAWD2			0x98
#define  ETAG_CTRL_P(p, x)		(((x) & 0x3) << ((p) << 1))
#define MT7530_SSP_P(x)		(0x2000 + ((x) * 0x100))
#define  FID_PST(fid, state)		(((state) & 0x3) << ((fid) * 2))
#define  FID_PST_MASK(fid)		FID_PST(fid, 0x3)
#define MT7530_PCR_P(x)		(0x2004 + ((x) * 0x100))
#define  PCR_MATRIX(x)			(((x) & 0xff) << 16)
#define  PCR_MATRIX_MASK		PCR_MATRIX(0xff)
#define  PCR_PORT_VLAN_MASK		0x3
#define MT7530_PVC_P(x)		(0x2010 + ((x) * 0x100))
#define  PORT_SPEC_TAG			BIT(5)
#define  PVC_EG_TAG(x)		(((x) & 0x7) << 8)
#define  PVC_EG_TAG_MASK		PVC_EG_TAG(7)
#define  VLAN_ATTR(x)			(((x) & 0x3) << 6)
#define  VLAN_ATTR_MASK		VLAN_ATTR(3)
#define  ACC_FRM_MASK			GENMASK(1, 0)
#define MT7530_PPBV1_P(x)		(0x2014 + ((x) * 0x100))
#define  G0_PORT_VID(x)		(((x) & 0xfff) << 0)
#define  G0_PORT_VID_MASK		G0_PORT_VID(0xfff)
#define EN75_MT7530_PVC_DEFAULT	0x81000000

enum en75_mt7530_vlan_cmd {
	MT7530_VTCR_WR_VID = 1,
};

enum en75_mt7530_fid {
	FID_BRIDGED = 1,
};

enum en75_mt7530_stp_state {
	MT7530_STP_FORWARDING = 3,
};

enum en75_mt7530_port_mode {
	MT7530_PORT_SECURITY_MODE = 3,
};

enum en75_mt7530_vlan_egress_attr {
	MT7530_VLAN_EGRESS_UNTAG = 0,
	MT7530_VLAN_EGRESS_TAG = 2,
};

enum en75_mt7530_vlan_port_eg_tag {
	MT7530_VLAN_EG_DISABLED = 0,
	MT7530_VLAN_EG_CONSISTENT = 1,
};

enum en75_mt7530_vlan_port_attr {
	MT7530_VLAN_USER = 0,
};

enum en75_mt7530_vlan_port_acc_frm {
	MT7530_VLAN_ACC_ALL = 0,
};

struct en75_eth_pvt {
	struct en75_eth			pub;
	const struct en75_soc_data	*soc;
	struct net_device		*ports[EN75_NUM_GDM_PORTS];
	struct en751221_regs __iomem	*regs;
	struct reset_control		*reset;
	int				qdma_irq[EN75_NUM_QDMA * QDMA_NUM_IRQS];
	struct en75_qdma		*qdma[EN75_NUM_QDMA];
	struct en75_debug		*debug;
	struct delayed_work		mt7530_reapply_work;
	unsigned int			mt7530_reapply_count;
	/* Whether our register window covers the on-die switch, see probe. */
	bool				has_switch_regs;
};

union gdm_regs {
	struct gdm	regs;
	u32		words[0x800 / sizeof(u32)];
};

struct en751221_regs {
	/* BFB50000 - BFB50400 */
	u32		fe[0x400 / sizeof(u32)];

	/* BFB50400 - BFB50C00 */
	union gdm_regs	port0;

	/* BFB50C00 - BFB51000 */
	u32		ppe[0x400 / sizeof(u32)];

	/* BFB51000 - BFB51400 */
	u32		unknown_deadbeef[0x400 / sizeof(u32)];

	/* BFB51400 - BFB51C00 */
	union gdm_regs	port1;

	/* BFB51C00 - BFB52000 */
	u32		unknown_deadbeef2[0x400 / sizeof(u32)];

	/* BFB52000 - BFB52400 */
	u32		ppe_accounting[0x400 / sizeof(u32)];

	/* BFB52400 - BFB54000 */
	u32		ppe_unused[0x1c00 / sizeof(u32)];

	/* BFB54000 - BFB56000 */
	struct qregs	qdma_regs[EN75_NUM_QDMA];

	/* BFB56000 - BFB58000 */
	u32		unknown_zeroed[0x2000 / sizeof(u32)];

	/* BFB58000 - BFB60000 */
	u32		switch_regs[0x8000 / sizeof(u32)];
};
_Static_assert(sizeof(struct en751221_regs) == 0x10000, "en751221_regs size incorrect");

static u32 en75_gsw_read(struct en75_eth_pvt *eth, u32 reg)
{
	return readl(&eth->regs->switch_regs[reg / sizeof(u32)]);
}

static void en75_gsw_write(struct en75_eth_pvt *eth, u32 reg, u32 val)
{
	writel(val, &eth->regs->switch_regs[reg / sizeof(u32)]);
}

static int en75_mdio_wait(struct en75_eth_pvt *eth)
{
	u32 val;
	int err;

	err = read_poll_timeout(en75_gsw_read, val, !(val & PHY_IAC_ACCESS),
				20, 20000, false, eth, EN75_PHY_IAC);
	if (err)
		dev_err(eth->pub.dev, "timeout waiting for switch MDIO master\n");

	return err;
}

static int en75_mdio_write16(struct en75_eth_pvt *eth, u32 phy, u32 reg,
				     u16 val)
{
	int err;

	err = en75_mdio_wait(eth);
	if (err)
		return err;

	en75_gsw_write(eth, EN75_PHY_IAC,
		       PHY_IAC_ACCESS | PHY_IAC_START | PHY_IAC_WRITE |
		       PHY_IAC_REG(reg) | PHY_IAC_ADDR(phy) | val);

	return en75_mdio_wait(eth);
}

static int en75_mdio_read16(struct en75_eth_pvt *eth, u32 phy, u32 reg,
				    u16 *val)
{
	int err;

	err = en75_mdio_wait(eth);
	if (err)
		return err;

	en75_gsw_write(eth, EN75_PHY_IAC,
		       PHY_IAC_ACCESS | PHY_IAC_START | PHY_IAC_READ |
		       PHY_IAC_REG(reg) | PHY_IAC_ADDR(phy));

	err = en75_mdio_wait(eth);
	if (err)
		return err;

	*val = en75_gsw_read(eth, EN75_PHY_IAC) & PHY_IAC_DATA_MASK;

	return 0;
}

static int en75_ext_switch_write(struct en75_eth_pvt *eth, u32 reg, u32 val)
{
	int err;

	err = en75_mdio_write16(eth, EN75_MT7530_MDIO_ADDR, 0x1f,
				  (reg >> 6) & 0x3ff);
	if (err)
		return err;

	err = en75_mdio_write16(eth, EN75_MT7530_MDIO_ADDR,
				  (reg >> 2) & 0xf, val & 0xffff);
	if (err)
		return err;

	return en75_mdio_write16(eth, EN75_MT7530_MDIO_ADDR, 0x10, val >> 16);
}

static int en75_ext_switch_read(struct en75_eth_pvt *eth, u32 reg, u32 *val)
{
	u16 high, low;
	int err;

	err = en75_mdio_write16(eth, EN75_MT7530_MDIO_ADDR, 0x1f,
				  (reg >> 6) & 0x3ff);
	if (err)
		return err;

	err = en75_mdio_read16(eth, EN75_MT7530_MDIO_ADDR,
				 (reg >> 2) & 0xf, &low);
	if (err)
		return err;

	err = en75_mdio_read16(eth, EN75_MT7530_MDIO_ADDR, 0x10, &high);
	if (err)
		return err;

	*val = ((u32)high << 16) | low;

	return 0;
}

static u32 en75_ext_switch_read_poll(struct en75_eth_pvt *eth, u32 reg)
{
	u32 val = 0;

	en75_ext_switch_read(eth, reg, &val);

	return val;
}

static int en75_ext_switch_rmw(struct en75_eth_pvt *eth, u32 reg, u32 mask,
				       u32 val)
{
	u32 orig;
	int err;

	err = en75_ext_switch_read(eth, reg, &orig);
	if (err)
		return err;

	return en75_ext_switch_write(eth, reg, (orig & ~mask) | val);
}

static int en75_mmd_write(struct en75_eth_pvt *eth, u32 phy, u32 dev,
				  u32 reg, u16 val)
{
	int err;

	/* Vendor mtEMiiRegWrite(): CL22 page 0 + MMD ctrl/data registers. */
	err = en75_mdio_write16(eth, phy, 0x1f, 0x0000);
	if (err)
		return err;
	err = en75_mdio_write16(eth, phy, 0x0d, dev);
	if (err)
		return err;
	err = en75_mdio_write16(eth, phy, 0x0e, reg);
	if (err)
		return err;
	err = en75_mdio_write16(eth, phy, 0x0d, BIT(14) | dev);
	if (err)
		return err;

	return en75_mdio_write16(eth, phy, 0x0e, val);
}

static int en75_trgmii_setup_clock(struct en75_eth_pvt *eth, u32 phy)
{
	int err;

	err = en75_mmd_write(eth, phy, 0x1f, 0x404, 0x1000);
	if (err)
		return err;
	err = en75_mmd_write(eth, phy, 0x1f, 0x409, 0x0057);
	if (err)
		return err;
	err = en75_mmd_write(eth, phy, 0x1f, 0x40a, 0x0057);
	if (err)
		return err;
	err = en75_mmd_write(eth, phy, 0x1f, 0x403, 0x1800);
	if (err)
		return err;
	mdelay(5);
	err = en75_mmd_write(eth, phy, 0x1f, 0x403, 0x1c00);
	if (err)
		return err;
	err = en75_mmd_write(eth, phy, 0x1f, 0x401, 0xc020);
	if (err)
		return err;
	err = en75_mmd_write(eth, phy, 0x1f, 0x406, 0xa030);
	if (err)
		return err;
	err = en75_mmd_write(eth, phy, 0x1f, 0x406, 0xa038);
	if (err)
		return err;

	return en75_mmd_write(eth, phy, 0x1f, 0x410, 0x0003);
}

/*
 * Re-synchronise the TRGMII link between the SoC GSW (port 5) and the external
 * MT7530 (port 6). Some Archer C5v boots end up with correct MT7530 registers
 * but no RX traffic crossing the CPU/TRGMII path. This mirrors the vendor SDK
 * pad-resync sequence from trgmii_interface_init(). Cold power-on also needs
 * the vendor 200MHz MMD clock setup before the pads are reset.
 */
#define EN75_TRGMII_PMCR_DOWN \
	((u32)(EN75_PMCR_CONFIG & ~(PMCR_FORCE_LNK | PMCR_MAC_TX_EN)))

static int en75_trgmii_resync(struct en75_eth_pvt *eth)
{
	static const u16 odt_regs[] = { 0x7a54, 0x7a5c, 0x7a64, 0x7a6c, 0x7a74 };
	u32 reg;
	int i;
	int err;

	/* Vendor SDK disables external switch flow control before TRGMII setup. */
	err = en75_ext_switch_rmw(eth, 0x1fe0, BIT(31), 0);
	if (err)
		return err;

	/* MT7530 7-second auto-powerdown workaround (CKGCR). */
	err = en75_ext_switch_write(eth, 0x30f0, 0x00001e02);
	if (err)
		return err;

	/* Enable TRGMII on both endpoints. */
	err = en75_ext_switch_write(eth, 0x7808, 0);
	if (err)
		return err;
	err = en75_ext_switch_write(eth, 0x7804, 0x01017e8f);
	if (err)
		return err;
	err = en75_ext_switch_write(eth, 0x7808, 1);
	if (err)
		return err;

	en75_gsw_write(eth, 0x7808, 0);
	en75_gsw_write(eth, 0x7804, 0x01017e8f);
	en75_gsw_write(eth, 0x7808, 1);

	/* Setup TRGMII 200MHz clock/PLL on both endpoints, as in vendor GPL. */
	err = en75_trgmii_setup_clock(eth, 0);
	if (err)
		return err;
	err = en75_trgmii_setup_clock(eth, 12);
	if (err)
		return err;
	mdelay(50);

	/* Force both CPU-port links down while the pads are reset. */
	en75_gsw_write(eth, MT753X_PMCR_P(5), EN75_TRGMII_PMCR_DOWN);
	err = en75_ext_switch_write(eth, MT753X_PMCR_P(6), EN75_TRGMII_PMCR_DOWN);
	if (err)
		return err;
	mdelay(5);

	/* Reset TRGMII Tx/Rx (TXCTRL 0x7a40 bit 28). */
	err = en75_ext_switch_read(eth, 0x7a40, &reg);
	if (err)
		return err;
	err = en75_ext_switch_write(eth, 0x7a40, reg | BIT(28));
	if (err)
		return err;
	mdelay(5);
	err = en75_ext_switch_write(eth, 0x7a40, reg & ~BIT(28));
	if (err)
		return err;

	reg = en75_gsw_read(eth, 0x7a40);
	en75_gsw_write(eth, 0x7a40, reg | BIT(28));
	mdelay(5);
	en75_gsw_write(eth, 0x7a40, reg & ~BIT(28));

	/* Enter calibration (RCK_CTRL 0x7a00 bit 31). */
	err = en75_ext_switch_read(eth, 0x7a00, &reg);
	if (err)
		return err;
	err = en75_ext_switch_write(eth, 0x7a00, reg | BIT(31));
	if (err)
		return err;
	reg = en75_gsw_read(eth, 0x7a00);
	en75_gsw_write(eth, 0x7a00, reg | BIT(31));

	/* Tx driving strength / ODT board values from the vendor SDK. */
	for (i = 0; i < ARRAY_SIZE(odt_regs); i++) {
		err = en75_ext_switch_write(eth, odt_regs[i], 0xaa);
		if (err)
			return err;
		en75_gsw_write(eth, odt_regs[i], 0xaa);
	}
	err = en75_ext_switch_write(eth, 0x7a7c, 0x77);
	if (err)
		return err;
	en75_gsw_write(eth, 0x7a7c, 0x77);

	/* Enable the TRGMII port. */
	err = en75_ext_switch_write(eth, 0x7830, 1);
	if (err)
		return err;
	en75_gsw_write(eth, 0x7830, 1);
	mdelay(5);

	/* Leave calibration. */
	err = en75_ext_switch_read(eth, 0x7a00, &reg);
	if (err)
		return err;
	err = en75_ext_switch_write(eth, 0x7a00, reg & ~BIT(31));
	if (err)
		return err;
	reg = en75_gsw_read(eth, 0x7a00);
	en75_gsw_write(eth, 0x7a00, reg & ~BIT(31));

	/* Bring both CPU-port links back up with TX/RX enabled. */
	en75_gsw_write(eth, MT753X_PMCR_P(5), EN75_PMCR_CONFIG);
	err = en75_ext_switch_write(eth, MT753X_PMCR_P(6), EN75_PMCR_CONFIG);
	if (err)
		return err;

	dev_info(eth->pub.dev, "re-synced MT7530 TRGMII path\n");

	return 0;
}

static int en75_ext_switch_vlan_cmd(struct en75_eth_pvt *eth, u16 vid)
{
	struct device *dev = eth->pub.dev;
	u32 val;
	int err;

	err = en75_ext_switch_write(eth, MT7530_VTCR,
				     VTCR_BUSY | VTCR_FUNC(MT7530_VTCR_WR_VID) |
				     vid);
	if (err)
		return err;

	err = read_poll_timeout(en75_ext_switch_read_poll, val,
				!(val & VTCR_BUSY), 20, 20000, false, eth,
				MT7530_VTCR);
	if (err)
		dev_err(dev, "timeout writing external switch VLAN %u\n", vid);

	return err;
}

static int en75_ext_switch_write_vlan(struct en75_eth_pvt *eth, u16 vid,
				      u8 members, u8 untagged)
{
	u32 vawd2 = 0;
	int port;
	int err;

	for (port = 0; port < EN75_MT7530_NUM_PORTS; port++) {
		if (!(members & BIT(port)))
			continue;

		vawd2 |= ETAG_CTRL_P(port,
				     (untagged & BIT(port)) ?
				     MT7530_VLAN_EGRESS_UNTAG :
				     MT7530_VLAN_EGRESS_TAG);
	}

	err = en75_ext_switch_write(eth, MT7530_VAWD1,
				     IVL_MAC | VTAG_EN | PORT_MEM(members) |
				     FID(FID_BRIDGED) | VLAN_VALID);
	if (err)
		return err;

	err = en75_ext_switch_write(eth, MT7530_VAWD2, vawd2);
	if (err)
		return err;

	return en75_ext_switch_vlan_cmd(eth, vid);
}

static int en75_ext_switch_set_port(struct en75_eth_pvt *eth, int port,
				    u8 members, u16 pvid, bool cpu_port)
{
	u8 port_matrix = cpu_port ? 0xff : members;
	u32 eg_tag;
	int err;

	err = en75_ext_switch_rmw(eth, MT7530_SSP_P(port),
				   FID_PST_MASK(FID_BRIDGED),
				   FID_PST(FID_BRIDGED, MT7530_STP_FORWARDING));
	if (err)
		return err;

	err = en75_ext_switch_rmw(eth, MT7530_PCR_P(port),
				   PCR_MATRIX_MASK | PCR_PORT_VLAN_MASK,
				   PCR_MATRIX(port_matrix) | MT7530_PORT_SECURITY_MODE);
	if (err)
		return err;

	eg_tag = cpu_port ? MT7530_VLAN_EG_CONSISTENT :
			    MT7530_VLAN_EG_DISABLED;
	err = en75_ext_switch_rmw(eth, MT7530_PVC_P(port),
				   VLAN_ATTR_MASK | PVC_EG_TAG_MASK | ACC_FRM_MASK |
				   PORT_SPEC_TAG,
				   VLAN_ATTR(MT7530_VLAN_USER) | PVC_EG_TAG(eg_tag) |
				   MT7530_VLAN_ACC_ALL);
	if (err)
		return err;

	return en75_ext_switch_rmw(eth, MT7530_PPBV1_P(port),
				     G0_PORT_VID_MASK, G0_PORT_VID(pvid));
}

static int en75_ext_switch_write_port_compat(struct en75_eth_pvt *eth,
						   const u32 *lan_ports, int lan_count,
						   u32 cpu_port, u32 wan_port,
						   u16 lan_vid, u16 wan_untagged_vid)
{
	int i, err;

	for (i = 0; i < lan_count; i++) {
		err = en75_ext_switch_write(eth, MT7530_PCR_P(lan_ports[i]),
					    PCR_MATRIX(0xff) | MT7530_PORT_SECURITY_MODE);
		if (err)
			return err;

		err = en75_ext_switch_write(eth, MT7530_PPBV1_P(lan_ports[i]),
					    G0_PORT_VID(lan_vid));
		if (err)
			return err;

		err = en75_ext_switch_write(eth, MT7530_PVC_P(lan_ports[i]),
					    EN75_MT7530_PVC_DEFAULT);
		if (err)
			return err;
	}

	err = en75_ext_switch_write(eth, MT7530_PCR_P(wan_port),
					PCR_MATRIX(0xff) | MT7530_PORT_SECURITY_MODE);
	if (err)
		return err;

	err = en75_ext_switch_write(eth, MT7530_PPBV1_P(wan_port),
					G0_PORT_VID(wan_untagged_vid));
	if (err)
		return err;

	err = en75_ext_switch_write(eth, MT7530_PVC_P(wan_port),
					EN75_MT7530_PVC_DEFAULT);
	if (err)
		return err;

	err = en75_ext_switch_write(eth, MT7530_PCR_P(cpu_port),
					PCR_MATRIX(0xff) | MT7530_PORT_SECURITY_MODE);
	if (err)
		return err;

	return en75_ext_switch_write(eth, MT7530_PVC_P(cpu_port),
				       EN75_MT7530_PVC_DEFAULT);
}

static int en75_ext_switch_configure_vlan_split(struct en75_eth_pvt *eth)
{
	struct device_node *np = eth->pub.dev->of_node;
	u32 lan_ports[EN75_MT7530_NUM_PORTS] = { 0, 1, 2, 3 };
	u32 cpu_port = 6, lan_vid = 1, wan_port = 4, wan_vid = 35;
	u32 iptv_port = EN75_MT7530_NUM_PORTS, iptv_vid = 0;
	u32 wan_untagged_vid = EN75_MT7530_WAN_UNTAGGED_VID;
	bool wan_untagged;
	bool iptv = false;
	bool iptv_cpu;
	int lan_count;
	u8 lan_mask = 0;
	u8 wan_mask;
	u8 iptv_mask = 0;
	u8 iptv_cpu_mask = 0;
	u8 cpu_mask;
	int i, err;

	if (!of_property_read_bool(np, "econet,mt7530-vlan-split"))
		return 0;

	of_property_read_u32(np, "econet,mt7530-cpu-port", &cpu_port);
	of_property_read_u32(np, "econet,mt7530-lan-vid", &lan_vid);
	of_property_read_u32(np, "econet,mt7530-wan-port", &wan_port);
	of_property_read_u32(np, "econet,mt7530-wan-vid", &wan_vid);
	of_property_read_u32(np, "econet,mt7530-wan-untagged-vid",
			     &wan_untagged_vid);
	wan_untagged = of_property_read_bool(np, "econet,mt7530-wan-untagged");
	iptv_cpu = of_property_read_bool(np, "econet,mt7530-iptv-cpu-tagged");
	err = of_property_read_u32(np, "econet,mt7530-iptv-port", &iptv_port);
	if (!err) {
		iptv = true;
		err = of_property_read_u32(np, "econet,mt7530-iptv-vid",
					   &iptv_vid);
		if (err)
			return dev_err_probe(eth->pub.dev, err,
					     "missing external MT7530 IPTV VID\n");
	} else if (err != -EINVAL) {
		return err;
	}

	lan_count = of_property_count_u32_elems(np, "econet,mt7530-lan-ports");
	if (lan_count > 0) {
		if (lan_count > ARRAY_SIZE(lan_ports))
			return dev_err_probe(eth->pub.dev, -EINVAL,
					     "too many external MT7530 LAN ports\n");

		err = of_property_read_u32_array(np, "econet,mt7530-lan-ports",
						 lan_ports, lan_count);
		if (err)
			return err;
	} else {
		lan_count = 4;
	}

	if (cpu_port >= EN75_MT7530_NUM_PORTS ||
	    wan_port >= EN75_MT7530_NUM_PORTS ||
	    (iptv && iptv_port >= EN75_MT7530_NUM_PORTS) ||
	    lan_vid > 4095 || wan_vid > 4095 || wan_untagged_vid > 4095 ||
	    (iptv && iptv_vid > 4095) ||
	    lan_vid == wan_vid || lan_vid == wan_untagged_vid ||
	    wan_vid == wan_untagged_vid ||
	    (iptv && (iptv_vid == lan_vid || iptv_vid == wan_vid ||
		      iptv_vid == wan_untagged_vid ||
		      iptv_port == cpu_port || iptv_port == wan_port)))
		return dev_err_probe(eth->pub.dev, -EINVAL,
				     "invalid external MT7530 VLAN split config\n");

	cpu_mask = BIT(cpu_port);
	wan_mask = BIT(wan_port);
	if (iptv) {
		iptv_mask = BIT(iptv_port);
		if (iptv_cpu)
			iptv_cpu_mask = cpu_mask;
	}

	for (i = 0; i < lan_count; i++) {
		if (lan_ports[i] >= EN75_MT7530_NUM_PORTS ||
		    lan_ports[i] == cpu_port || lan_ports[i] == wan_port ||
		    (iptv && lan_ports[i] == iptv_port))
			return dev_err_probe(eth->pub.dev, -EINVAL,
					     "invalid external MT7530 LAN port\n");

		lan_mask |= BIT(lan_ports[i]);
	}

	err = en75_ext_switch_write(eth, MT753X_PMCR_P(cpu_port),
				     EN75_PMCR_CONFIG);
	if (err)
		return err;

	err = en75_ext_switch_write(eth, MT753X_MFC, EN75_MFC_CONFIG);
	if (err)
		return err;

	for (i = 0; i < lan_count; i++) {
		err = en75_ext_switch_set_port(eth, lan_ports[i],
					       lan_mask | cpu_mask, lan_vid, false);
		if (err)
			return err;
	}

	err = en75_ext_switch_set_port(eth, wan_port,
				       wan_mask | cpu_mask | iptv_mask,
				       wan_untagged_vid, false);
	if (err)
		return err;

	if (iptv) {
		err = en75_ext_switch_set_port(eth, iptv_port,
					       wan_mask | iptv_mask | iptv_cpu_mask,
					       iptv_vid, false);
		if (err)
			return err;
	}

	err = en75_ext_switch_set_port(eth, cpu_port,
				       lan_mask | wan_mask |
				       (iptv_cpu ? iptv_mask : 0), 0, true);
	if (err)
		return err;

	err = en75_ext_switch_write_vlan(eth, lan_vid, lan_mask | cpu_mask,
					 lan_mask);
	if (err)
		return err;

	err = en75_ext_switch_write_vlan(eth, wan_untagged_vid,
					 wan_mask | cpu_mask, wan_mask);
	if (err)
		return err;

	err = en75_ext_switch_write_vlan(eth, wan_vid, wan_mask | cpu_mask,
					 wan_untagged ? wan_mask : 0);
	if (err)
		return err;

	if (iptv) {
		err = en75_ext_switch_write_vlan(eth, iptv_vid,
						 wan_mask | iptv_mask |
						 iptv_cpu_mask,
						 iptv_mask);
		if (err)
			return err;
	}

	err = en75_ext_switch_write_port_compat(eth, lan_ports, lan_count,
					       cpu_port, wan_port, lan_vid,
					       wan_untagged_vid);
	if (err)
		return err;

	dev_info(eth->pub.dev,
		 "configured external MT7530 VLAN split: lan vid %u ports 0x%x, wan untagged vid %u, wan tagged vid %u port %u, cpu port %u%s\n",
		 lan_vid, lan_mask, wan_untagged_vid, wan_vid, wan_port, cpu_port,
		 wan_untagged ? " untagged" : " tagged");
	if (iptv && iptv_cpu)
		dev_info(eth->pub.dev,
			 "configured external MT7530 IPTV bridge: vid %u wan port %u tagged, iptv port %u untagged, cpu port %u tagged\n",
			 iptv_vid, wan_port, iptv_port, cpu_port);
	else if (iptv)
		dev_info(eth->pub.dev,
			 "configured external MT7530 IPTV bridge: vid %u wan port %u tagged, iptv port %u untagged, cpu port omitted\n",
			 iptv_vid, wan_port, iptv_port);

	return 0;
}

#define EN75_MT7530_REAPPLY_MAX	5

static unsigned int en75_ext_switch_reapply_delay_ms(unsigned int attempt)
{
	switch (attempt) {
	case 0:
		return 2000;
	case 1:
		return 10000;
	case 2:
		return 30000;
	case 3:
		return 60000;
	default:
		return 120000;
	}
}

static bool en75_ext_switch_rx_seen(struct en75_eth_pvt *eth)
{
	struct rtnl_link_stats64 stats;
	int i;

	for (i = 0; i < ARRAY_SIZE(eth->ports); i++) {
		if (!eth->ports[i])
			continue;

		memset(&stats, 0, sizeof(stats));
		dev_get_stats(eth->ports[i], &stats);
		if (stats.rx_packets)
			return true;
	}

	return false;
}

static void en75_ext_switch_reapply_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct en75_eth_pvt *eth = container_of(dwork, struct en75_eth_pvt,
						 mt7530_reapply_work);
	unsigned int attempt = eth->mt7530_reapply_count + 1;
	int err;

	if (en75_ext_switch_rx_seen(eth)) {
		dev_info(eth->pub.dev,
			 "MT7530 RX traffic seen, stopping delayed TRGMII/VLAN reapply\n");
		eth->mt7530_reapply_count = EN75_MT7530_REAPPLY_MAX;
		return;
	}

	err = en75_trgmii_resync(eth);
	if (err)
		dev_warn(eth->pub.dev,
			 "failed to re-sync MT7530 TRGMII path attempt %u: %d\n",
			 attempt, err);

	err = en75_ext_switch_configure_vlan_split(eth);
	if (err)
		dev_warn(eth->pub.dev,
			 "failed to re-apply external MT7530 TRGMII/VLAN split attempt %u: %d\n",
			 attempt, err);
	else
		dev_info(eth->pub.dev,
			 "re-applied external MT7530 TRGMII/VLAN split attempt %u/%u\n",
			 attempt, EN75_MT7530_REAPPLY_MAX);

	eth->mt7530_reapply_count++;
	if (eth->mt7530_reapply_count < EN75_MT7530_REAPPLY_MAX)
		schedule_delayed_work(&eth->mt7530_reapply_work,
			msecs_to_jiffies(en75_ext_switch_reapply_delay_ms(
				eth->mt7530_reapply_count)));
}

static void en75_ext_switch_schedule_reapply(struct en75_eth_pvt *eth)
{
	if (!of_property_read_bool(eth->pub.dev->of_node,
				   "econet,mt7530-vlan-split"))
		return;

	eth->mt7530_reapply_count = 0;
	schedule_delayed_work(&eth->mt7530_reapply_work,
		msecs_to_jiffies(en75_ext_switch_reapply_delay_ms(0)));
}

static struct net_device *en75_get_sport_dev(struct en75_eth_pvt *eth,
					     enum etx_fport sport)
{
	if (sport == ETX_FPORT_GDM2) {
		return eth->ports[1];
	} else {
		if (sport != ETX_FPORT_GDM1)
			dev_info(eth->pub.dev, "rx: on unexpected fport %d\n", sport);
		return eth->ports[0];
	}
}

int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb,
			enum etx_fport sport)
{
	struct en75_eth_pvt *ep = (struct en75_eth_pvt *) eth;
	struct net_device *port;

	port = en75_get_sport_dev(ep, sport);

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
		u32 sptag = get_erx_sp_tag(&desc.t.erx);
		if (sptag < ARRAY_SIZE(port->dsa_meta) &&
			port->dsa_meta[sptag])
			skb_dst_set_noref(q->skb,
						&port->dsa_meta[sptag]->dst);
		#endif
	}

	return 0;
}

int en75_port_set_macaddr(struct en75_eth *eth, enum etx_fport portn, const u8 *addr)
{
	struct en75_eth_pvt *ep = (struct en75_eth_pvt *) eth;
	u32 __iomem *reg = ep->regs->switch_regs;
	struct gdm_mymac_msb msb = { .word = 0 };
	struct gdm_mymac_lsb lsb = { .word = 0 };

	/*
	 * This programs the switch source MAC address (SMACCR). Those
	 * registers live in the switch register block, so when the switch is
	 * managed by the mt7530 DSA driver they are not ours to write.
	 */
	if (!ep->has_switch_regs)
		return 0;

	if (portn != ETX_FPORT_GDM1)
		return 0;

	set_gdm_mymac_msb_a(&msb, addr[0]);
	set_gdm_mymac_msb_b(&msb, addr[1]);
	set_gdm_mymac_lsb_c(&lsb, addr[2]);
	set_gdm_mymac_lsb_d(&lsb, addr[3]);
	set_gdm_mymac_lsb_e(&lsb, addr[4]);
	set_gdm_mymac_lsb_f(&lsb, addr[5]);

	en75_wreg(lsb, (struct gdm_mymac_lsb *)&reg[SWITCH_MAC_LO / 4]);
	en75_wreg(msb, (struct gdm_mymac_msb *)&reg[SWITCH_MAC_HI / 4]);

	return 0;
}

static int en75_init_port(struct en75_eth_pvt *eth, struct device_node *np)
{
	const __be32 *id_ptr = of_get_property(np, "reg", NULL);
	struct net_device *dev;
	u32 id;

	if (!id_ptr) {
		dev_err(eth->pub.dev, "missing gdm port id\n");
		return -EINVAL;
	}

	id = be32_to_cpup(id_ptr) + 1;

	if (!id || id > ARRAY_SIZE(eth->ports)) {
		dev_err(eth->pub.dev, "invalid gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (eth->ports[id - 1]) {
		dev_err(eth->pub.dev, "duplicate gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (id == 1)
		dev = en75_alloc_gdm_port(&eth->pub, np,
					  &eth->regs->port0.regs,
					  eth->qdma[0],
					  ETX_FPORT_GDM1,
					  false);
	else if (id == 2)
		dev = en75_alloc_gdm_port(&eth->pub, np,
					  &eth->regs->port1.regs,
					  eth->qdma[1],
					  ETX_FPORT_GDM2,
					  true);
	else
		return -EINVAL;

	if (IS_ERR(dev))
		return PTR_ERR(dev);
	
	eth->ports[id - 1] = dev;
	return 0;
}

static void en75_prepare_qdma_cfg(struct en75_qdma_cfg *cfg,
				  const struct en75_soc_data *soc)
{
	memset(cfg, 0, sizeof(*cfg));
	for (int i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_rx_descs[i] = 128;
	for (int i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_tx_descs[i] = 128;
	for (int i = 0; i < QDMA_NUM_TX_DONE; i++)
		cfg->done_list_size[i] = 256;
	cfg->fwd_max_packet_size = EN75_MAX_PACKET_SIZE;
	cfg->fwd_low_threshold = 32;
	cfg->num_fwd_descs = 256;
	cfg->soc = soc;
}

static void en75_remove(struct platform_device *pdev)
{
	struct en75_eth_pvt *eth = platform_get_drvdata(pdev);
	int i;

	if (!eth)
		return;

	cancel_delayed_work_sync(&eth->mt7530_reapply_work);

	en75_debugfs_exit(eth->debug);

	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++)
		if (eth->qdma[i])
			en75_qdma_destroy(eth->qdma[i]);

	for (i = 0; i < ARRAY_SIZE(eth->ports); i++) {
		if (!eth->ports[i])
			continue;

		unregister_netdev(eth->ports[i]);
	}

	platform_set_drvdata(pdev, NULL);
}

static int en75_probe(struct platform_device *pdev)
{
	struct en75_debug_conf debug_conf = {0};
	struct en751221_regs __iomem *regs;
	struct resource *regs_res;
	struct en75_eth_pvt *eth;
	struct en75_qdma_cfg cfg;
	struct device_node *np;
	int i, err, irq;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->soc = of_device_get_match_data(&pdev->dev);
	if (!eth->soc)
		return dev_err_probe(&pdev->dev, -EINVAL, "No matching SoC data\n");

	eth->pub.dev = &pdev->dev;
	platform_set_drvdata(pdev, eth);
	INIT_DELAYED_WORK(&eth->mt7530_reapply_work,
			  en75_ext_switch_reapply_work);

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return dev_err_probe(&pdev->dev, err, 
				     "failed setting DMA mask\n");

	regs = devm_platform_get_and_ioremap_resource(pdev, 0, &regs_res);
	if (IS_ERR(regs))
		return dev_err_probe(&pdev->dev, PTR_ERR(regs),
				     "failed to map registers\n");

	if (resource_size(regs_res) <
	    offsetof(struct en751221_regs, switch_regs)) {
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "insufficient register space\n");
	}

	/*
	 * The on-die switch sits right above the frame engine in the register
	 * space. When it is managed by the mt7530 DSA driver it is described
	 * by its own device tree node and claims those registers itself, so
	 * the window handed to us stops short of them. In that case we are
	 * purely the DSA conduit and must leave the switch alone.
	 */
	eth->has_switch_regs =
		resource_size(regs_res) >= sizeof(struct en751221_regs);

	eth->regs = regs;

	eth->reset = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(eth->reset)) {
		dev_warn(&pdev->dev, "failed to get resets %pe\n", eth->reset);
	} else {
		err = reset_control_assert(eth->reset);
		if (err)
			return err;

		msleep(20);
		err = reset_control_deassert(eth->reset);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(eth->qdma_irq); i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
					     "failed to get IRQ %d\n", i);
		eth->qdma_irq[i] = irq;
	}

	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma) != ARRAY_SIZE(regs->qdma_regs));
	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma) != ARRAY_SIZE(eth->qdma_irq) * QDMA_NUM_IRQS);
	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++) {
		en75_prepare_qdma_cfg(&cfg, eth->soc);
		eth->qdma[i] = en75_qdma_new(&eth->pub, &regs->qdma_regs[i], i,
					     &eth->qdma_irq[i * QDMA_NUM_IRQS],
					     QDMA_NUM_IRQS, &cfg,
					     &debug_conf.qdma[i]);

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

	eth->debug = en75_debugfs_init(&debug_conf);

	/* Configure the MT7530 as a dumb switch, unless it is managed by DSA */
	if (eth->has_switch_regs) {
		en75_wreg(EN75_PMCR_CONFIG, &regs->switch_regs[MT753X_PMCR_P(5) / 4]);
		en75_wreg(EN75_PMCR_CONFIG, &regs->switch_regs[MT753X_PMCR_P(6) / 4]);
		en75_wreg(EN75_MFC_CONFIG, &regs->switch_regs[MT753X_MFC / 4]);

		if (of_property_read_bool(pdev->dev.of_node, "econet,mt7530-vlan-split")) {
			err = en75_trgmii_resync(eth);
			if (err)
				dev_warn(&pdev->dev,
					 "failed to re-sync MT7530 TRGMII path: %d\n",
					 err);
		}

		err = en75_ext_switch_configure_vlan_split(eth);
		if (err)
			goto error;
	}

	en75_ext_switch_schedule_reapply(eth);

	return 0;

error:
	en75_remove(pdev);
	return err;
}

static const struct en75_soc_data en751221_soc_data = {
	.dscp_byte_swap = true,
};

static const struct en75_soc_data en7528_soc_data = {
	.dscp_byte_swap = false,
};

static const struct of_device_id of_en75_match[] = {
	{ .compatible = "econet,en751221-eth", .data = &en751221_soc_data },
	{ .compatible = "econet,en7528-eth", .data = &en7528_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_en75_match);

static struct platform_driver en75_driver = {
	.probe = en75_probe,
	.remove = en75_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_en75_match,
	},
};
module_platform_driver(en75_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Caleb James DeLisle <cjd@cjdns.fr>");
MODULE_DESCRIPTION("Ethernet driver for EcoNet SoC");
