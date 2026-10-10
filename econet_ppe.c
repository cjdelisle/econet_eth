// SPDX-License-Identifier: GPL-2.0-only
/*
 * Packet Processing Engine (PPE) bring-up for the EcoNet EN751221.
 *
 * See econet_ppe.h for the relationship to the mainline MediaTek PPE. This
 * file implements milestone 1 only: allocate the FOE table and enable the
 * engine with no bound flows. Flow binding (nf_flowtable offload) is added in
 * a later change.
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/if_ether.h>
#include <linux/in.h>

#include "econet_ppe.h"

#define EN75_PPE_WAIT_TIMEOUT_US	1000000

static inline u32 ppe_r32(struct en75_ppe *ppe, u32 reg)
{
	return readl(ppe->base + reg);
}

static inline void ppe_w32(struct en75_ppe *ppe, u32 reg, u32 val)
{
	writel(val, ppe->base + reg);
}

static int ppe_wait_busy(struct en75_ppe *ppe)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout(ppe->base + EN75_PPE_GLO_CFG, val,
				 !(val & EN75_PPE_GLO_CFG_BUSY),
				 20, EN75_PPE_WAIT_TIMEOUT_US);
	if (ret)
		dev_err(ppe->dev, "PPE table busy\n");

	return ret;
}

static void en75_ppe_cache_clear(struct en75_ppe *ppe)
{
	int i;

	/* Vendor PpeCleanCache751221 sequence: gate the cache off, issue the
	 * clear-all command, wait for the REQ bit to self-clear, gate the
	 * cache back on.
	 */
	ppe_w32(ppe, EN75_PPE_CACHE_EN,
		ppe_r32(ppe, EN75_PPE_CACHE_EN) & ~EN75_PPE_CACHE_EN_EBL);
	ppe_w32(ppe, EN75_PPE_CACHE_CTL, EN75_PPE_CACHE_CTL_CLEAR_ALL);
	for (i = 0; i < 1000; i++) {
		if (!(ppe_r32(ppe, EN75_PPE_CACHE_CTL) &
		      EN75_PPE_CACHE_CTL_REQ))
			break;
		udelay(1);
	}
	if (i == 1000)
		pr_warn_ratelimited("econet-eth: PPE cache clear timed out\n");
	ppe_w32(ppe, EN75_PPE_CACHE_EN,
		ppe_r32(ppe, EN75_PPE_CACHE_EN) | EN75_PPE_CACHE_EN_EBL);
}

void __en75_ppe_foe_commit(struct en75_ppe *ppe, u32 hash,
			   struct en75_foe_entry *entry)
{
	struct en75_foe_entry *hwe = &ppe->foe[hash];
	const u32 *src = (const u32 *)entry;
	u32 *dst = (u32 *)hwe;
	size_t i, words = sizeof(*hwe) / sizeof(u32);

	/* The FOE table is little-endian but this SoC is big-endian, so every
	 * 32-bit word of the entry (ib1, tuple, rewrite header, ib2) must be
	 * byte-swapped into the table. ib1 (word 0) holds the BIND state, so
	 * publish the body words first and ib1 last: the engine must never see
	 * a BIND-state entry whose body is half-written.
	 */
	for (i = 1; i < words; i++)
		dst[i] = cpu_to_le32(src[i]);
	wmb();
	dst[0] = cpu_to_le32(src[0]);
	dma_wmb();
	en75_ppe_cache_clear(ppe);
}

void __en75_ppe_foe_clear(struct en75_ppe *ppe, u32 hash)
{
	u32 ib1 = le32_to_cpu(ppe->foe[hash].ib1);

	ib1 &= ~EN75_FOE_IB1_STATE;
	ppe->foe[hash].ib1 = cpu_to_le32(ib1);
	dma_wmb();
	en75_ppe_cache_clear(ppe);
}

void en75_ppe_foe_commit(struct en75_ppe *ppe, u32 hash,
			 struct en75_foe_entry *entry)
{
	unsigned long flags;

	spin_lock_irqsave(&ppe->foe_lock, flags);
	__en75_ppe_foe_commit(ppe, hash, entry);
	spin_unlock_irqrestore(&ppe->foe_lock, flags);
}

void en75_ppe_foe_clear(struct en75_ppe *ppe, u32 hash)
{
	unsigned long flags;

	spin_lock_irqsave(&ppe->foe_lock, flags);
	__en75_ppe_foe_clear(ppe, hash);
	spin_unlock_irqrestore(&ppe->foe_lock, flags);
}

u16 en75_ppe_timestamp(struct en75_ppe *ppe)
{
	return readl(ppe->fe + EN75_FE_FOE_TS) & EN75_FOE_IB1_BIND_TIMESTAMP;
}

static const char *foe_state_name(u32 s)
{
	static const char * const n[] = { "invalid", "unbind", "bind", "fin" };

	return n[s & 0x3];
}

static const char *foe_pkt_type_name(u32 t)
{
	switch (t) {
	case EN75_PPE_PKT_TYPE_IPV4_HNAPT:	return "ipv4-hnapt";
	case EN75_PPE_PKT_TYPE_IPV4_ROUTE:	return "ipv4-route";
	case EN75_PPE_PKT_TYPE_BRIDGE:		return "bridge";
	case EN75_PPE_PKT_TYPE_IPV4_DSLITE:	return "ipv4-dslite";
	case EN75_PPE_PKT_TYPE_IPV6_ROUTE_3T:	return "ipv6-route-3t";
	case EN75_PPE_PKT_TYPE_IPV6_ROUTE_5T:	return "ipv6-route-5t";
	case EN75_PPE_PKT_TYPE_IPV6_6RD:	return "ipv6-6rd";
	default:				return "?";
	}
}

static int en75_ppe_foe_show(struct seq_file *m, void *v)
{
	struct en75_ppe *ppe = m->private;
	unsigned int counts[4] = { 0 };
	unsigned int i, shown = 0;

	for (i = 0; i < EN75_PPE_ENTRIES; i++) {
		u32 ib1 = le32_to_cpu(READ_ONCE(ppe->foe[i].ib1));

		counts[FIELD_GET(EN75_FOE_IB1_STATE, ib1)]++;
	}

	seq_printf(m, "FOE table @ %pad, %u entries\n",
		   &ppe->foe_phys, EN75_PPE_ENTRIES);
	seq_printf(m, "states: invalid=%u unbind=%u bind=%u fin=%u\n",
		   counts[EN75_FOE_STATE_INVALID], counts[EN75_FOE_STATE_UNBIND],
		   counts[EN75_FOE_STATE_BIND], counts[EN75_FOE_STATE_FIN]);

	for (i = 0; i < EN75_PPE_ENTRIES; i++) {
		u32 ib1 = le32_to_cpu(READ_ONCE(ppe->foe[i].ib1));
		u32 state = FIELD_GET(EN75_FOE_IB1_STATE, ib1);
		u32 type = FIELD_GET(EN75_FOE_IB1_PACKET_TYPE, ib1);

		if (state == EN75_FOE_STATE_INVALID)
			continue;
		if (shown++ >= 64) {
			seq_puts(m, "... (truncated)\n");
			break;
		}
		seq_printf(m, "  [%5u] %-6s %-13s ib1=%08x", i,
			   foe_state_name(state), foe_pkt_type_name(type), ib1);
		if (type == EN75_PPE_PKT_TYPE_IPV4_HNAPT ||
		    type == EN75_PPE_PKT_TYPE_IPV4_ROUTE) {
			struct en75_foe_ipv4 *e = &ppe->foe[i].ipv4;

			seq_printf(m, " orig sip=%08x dip=%08x ports=%08x",
				   le32_to_cpu(e->orig.src_ip),
				   le32_to_cpu(e->orig.dest_ip),
				   le32_to_cpu(e->orig.ports));
		}
		seq_putc(m, '\n');
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en75_ppe_foe);

/* Slot selected by "echo N > foe_slot"; read back with cat foe_slot. */
static u32 en75_ppe_foe_debug_slot;

static void en75_ppe_foe_dump_slot(struct seq_file *m, struct en75_ppe *ppe, u32 i)
{
	u32 ib1 = le32_to_cpu(READ_ONCE(ppe->foe[i].ib1));
	u32 state = FIELD_GET(EN75_FOE_IB1_STATE, ib1);
	u32 type = FIELD_GET(EN75_FOE_IB1_PACKET_TYPE, ib1);
	struct en75_foe_ipv4 *e;

	if (i >= EN75_PPE_ENTRIES) {
		seq_printf(m, "slot %u: out of range (max %u)\n",
			   i, EN75_PPE_ENTRIES - 1);
		return;
	}

	seq_printf(m, "[%5u] %-6s %-13s ib1=%08x\n", i,
		   foe_state_name(state), foe_pkt_type_name(type), ib1);
	if (state == EN75_FOE_STATE_INVALID) {
		seq_puts(m, "(entry invalid)\n");
		return;
	}

	if (type != EN75_PPE_PKT_TYPE_IPV4_HNAPT &&
	    type != EN75_PPE_PKT_TYPE_IPV4_ROUTE)
		return;

	e = &ppe->foe[i].ipv4;
	seq_printf(m, " orig sip=%08x dip=%08x ports=%08x\n",
		   le32_to_cpu(e->orig.src_ip), le32_to_cpu(e->orig.dest_ip),
		   le32_to_cpu(e->orig.ports));
	if (type == EN75_PPE_PKT_TYPE_IPV4_HNAPT)
		seq_printf(m, " new  sip=%08x dip=%08x ports=%08x ib2=%08x\n",
			   le32_to_cpu(e->new.src_ip), le32_to_cpu(e->new.dest_ip),
			   le32_to_cpu(e->new.ports), le32_to_cpu(e->ib2));

	{
		const u32 *raw = (const u32 *)&ppe->foe[i];
		u32 w11 = le32_to_cpu(READ_ONCE(raw[11]));
		u32 w12 = le32_to_cpu(READ_ONCE(raw[12]));
		u32 w13 = le32_to_cpu(READ_ONCE(raw[13]));
		u32 w14 = le32_to_cpu(READ_ONCE(raw[14]));
		u32 w15 = le32_to_cpu(READ_ONCE(raw[15]));
		u8 dst[ETH_ALEN] = {
			w12 >> 24, w12 >> 16, w12 >> 8, w12,
			w13 >> 24, w13 >> 16,
		};
		u8 src[ETH_ALEN] = {
			w14 >> 24, w14 >> 16, w14 >> 8, w14,
			w15 >> 24, w15 >> 16,
		};

		seq_printf(m, " l2raw w11=%08x w12=%08x w13=%08x w14=%08x w15=%08x\n",
			   w11, w12, w13, w14, w15);
		seq_printf(m, " l2dec dst=%pM src=%pM etype=%04x vlan1=%u vlan2=%u pppoe=%04x\n",
			   dst, src, w11 >> 16, w11 & 0xffff,
			   w13 & 0xffff, w15 & 0xffff);
	}
}

static int en75_ppe_foe_slot_show(struct seq_file *m, void *v)
{
	en75_ppe_foe_dump_slot(m, m->private, en75_ppe_foe_debug_slot);
	return 0;
}

static ssize_t en75_ppe_foe_slot_write(struct file *file,
				       const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	char buf[16] = {};
	unsigned long val;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	if (kstrtoul(buf, 0, &val) || val >= EN75_PPE_ENTRIES)
		return -EINVAL;

	en75_ppe_foe_debug_slot = val;
	return len;
}

static int en75_ppe_foe_slot_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_ppe_foe_slot_show, inode->i_private);
}

static const struct file_operations en75_ppe_foe_slot_fops = {
	.open		= en75_ppe_foe_slot_open,
	.read		= seq_read,
	.write		= en75_ppe_foe_slot_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * Debug facility: bind one synthetic, STATIC IPv4 HNAPT entry whose egress
 * PSE port is the CPU. The tuple uses RFC5737 TEST-NET addresses so it can
 * never match real traffic; the entry exists only to prove the commit path
 * (table write, hash, cache flush, persistence) end to end. A matching packet
 * would surface as crsn HIT_BIND_FORCE_CPU(0x16) without leaving the CPU path,
 * so there is no forwarding risk. `echo 1` binds, `echo 0` invalidates.
 */
static ssize_t en75_ppe_test_bind_write(struct file *file,
					const char __user *ubuf,
					size_t len, loff_t *off)
{
	static const u8 smac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x01 };
	static const u8 dmac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x02 };
	struct en75_ppe *ppe = file->private_data;
	struct en75_foe_entry e;
	char buf[4] = {};
	u32 hash;

	if (copy_from_user(buf, ubuf, min(len, sizeof(buf) - 1)))
		return -EFAULT;

	if (buf[0] == '0') {
		if (ppe->test_hash != ~0u) {
			u32 ib1 = le32_to_cpu(ppe->foe[ppe->test_hash].ib1);

			ppe->foe[ppe->test_hash].ib1 =
				cpu_to_le32(ib1 & ~EN75_FOE_IB1_STATE);
			dma_wmb();
			en75_ppe_cache_clear(ppe);
			ppe->test_hash = ~0u;
		}
		return len;
	}

	/* QDMA0_CPU (PSE port 0) keeps the bound packet on the CPU path. */
	en75_foe_entry_prepare(&e, EN75_PPE_PKT_TYPE_IPV4_HNAPT, IPPROTO_TCP,
			       0, smac, dmac);
	en75_foe_entry_set_ipv4_tuple(&e, false,
				      cpu_to_be32(0xc0000201), cpu_to_be16(1234),
				      cpu_to_be32(0xc0000202), cpu_to_be16(5678));
	en75_foe_entry_set_ipv4_tuple(&e, true,
				      cpu_to_be32(0xc6336401), cpu_to_be16(1234),
				      cpu_to_be32(0xc0000202), cpu_to_be16(5678));
	e.ib1 |= EN75_FOE_IB1_STATIC;

	hash = en75_foe_entry_hash(&e);
	en75_ppe_foe_commit(ppe, hash, &e);
	ppe->test_hash = hash;

	dev_info(ppe->dev, "test_bind: static HNAPT entry committed at hash %u\n",
		 hash);
	return len;
}

static const struct file_operations en75_ppe_test_bind_fops = {
	.open	= simple_open,
	.write	= en75_ppe_test_bind_write,
	.llseek	= default_llseek,
};

struct en75_ppe *en75_ppe_init(struct device *dev, void __iomem *base,
			       void __iomem *fe)
{
	struct en75_ppe *ppe;

	ppe = devm_kzalloc(dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return ERR_PTR(-ENOMEM);

	ppe->dev = dev;
	ppe->base = base;
	ppe->fe = fe;

	ppe->foe = dmam_alloc_coherent(dev,
				       EN75_PPE_ENTRIES * sizeof(*ppe->foe),
				       &ppe->foe_phys, GFP_KERNEL);
	if (!ppe->foe)
		return ERR_PTR(-ENOMEM);

	ppe->test_hash = ~0u;
	spin_lock_init(&ppe->foe_lock);
	ppe->debugfs = debugfs_create_dir("econet_ppe", NULL);
	debugfs_create_file("foe", 0444, ppe->debugfs, ppe,
			    &en75_ppe_foe_fops);
	debugfs_create_file("test_bind", 0200, ppe->debugfs, ppe,
			    &en75_ppe_test_bind_fops);
	debugfs_create_file("foe_slot", 0644, ppe->debugfs, ppe,
			    &en75_ppe_foe_slot_fops);

	if (en75_ppe_offload_init(ppe))
		dev_warn(dev, "flow offload table init failed\n");

	return ppe;
}

void en75_ppe_start(struct en75_ppe *ppe)
{
	u32 val;

	if (IS_ERR_OR_NULL(ppe))
		return;

	/* Point the engine at the (zeroed) FOE table. */
	ppe_w32(ppe, EN75_PPE_TB_BASE, ppe->foe_phys);

	val = EN75_PPE_TB_CFG_AGE_NON_L4 |
	      EN75_PPE_TB_CFG_AGE_UNBIND |
	      EN75_PPE_TB_CFG_AGE_TCP |
	      EN75_PPE_TB_CFG_AGE_UDP |
	      EN75_PPE_TB_CFG_AGE_TCP_FIN |
	      /* FORWARD: on a search miss the engine punts the packet to the CPU
	       * (crsn UN_HIT) WITHOUT auto-binding anything. We now compute the
	       * engine's exact hash slot in software (en75_ppe_ipv4_hash_slot,
	       * verified 30/30 on hardware) and write the BIND there, so the
	       * descriptor hint is no longer needed. FORWARD_BUILD was actively
	       * harmful: it auto-bound every miss to its own entry at the same
	       * hash slot, racing and overwriting the driver-built BIND (observed:
	       * bound>30 committed but only ~3 survived in the table, 0f never
	       * settling). With FORWARD there is no auto-bind to fight, so our
	       * BIND sticks and the engine hits it on the next packet.
	       */
	      FIELD_PREP(EN75_PPE_TB_CFG_SEARCH_MISS,
			 EN75_PPE_SEARCH_MISS_ACTION_FORWARD_BUILD) |
	      FIELD_PREP(EN75_PPE_TB_CFG_KEEPALIVE,
			 EN75_PPE_KEEPALIVE_DISABLE) |
	      FIELD_PREP(EN75_PPE_TB_CFG_HASH_MODE, 1) |
	      /* Do NOT set bit16: unlike mtk netsys there is no SCAN_MODE
	       * field at [17:16] on EN751221. Vendor hw_nat programs the FOE
	       * entry size as bit16/bit3 = 1/0 (32B), 0/0 (64B), 0/1 (80B).
	       * Setting bit16 together with ENTRY_80B made the engine walk the
	       * table with a 64-byte stride while the driver wrote 80-byte
	       * slots, so driver BINDs were never hit (only 0d/0e/0f, no 16).
	       */
	      FIELD_PREP(EN75_PPE_TB_CFG_ENTRY_NUM, EN75_PPE_ENTRIES_SHIFT) |
	      EN75_PPE_TB_CFG_ENTRY_80B;
	ppe_w32(ppe, EN75_PPE_TB_CFG, val);

	ppe_w32(ppe, EN75_PPE_IP_PROTO_CHK,
		EN75_PPE_IP_PROTO_CHK_IPV4 | EN75_PPE_IP_PROTO_CHK_IPV6);

	/* Enable the FOE cache; the table is freshly zeroed so nothing is
	 * stale to flush.
	 */
	ppe_w32(ppe, EN75_PPE_CACHE_EN, EN75_PPE_CACHE_EN_MAGIC);

	val = EN75_PPE_FLOW_CFG_IP6_3T_ROUTE |
	      EN75_PPE_FLOW_CFG_IP6_5T_ROUTE |
	      EN75_PPE_FLOW_CFG_IP6_6RD |
	      EN75_PPE_FLOW_CFG_IP4_NAT |
	      EN75_PPE_FLOW_CFG_IP4_NAPT |
	      EN75_PPE_FLOW_CFG_IP4_DSLITE |
	      EN75_PPE_FLOW_CFG_IP4_NAT_FRAG |
	      EN75_PPE_FLOW_CFG_IP4_TCP_FRAG |
	      EN75_PPE_FLOW_CFG_IP4_UDP_FRAG |
	      EN75_PPE_FLOW_CFG_UDP_IP4 |
	      EN75_PPE_FLOW_CFG_FOE_EBL;
	ppe_w32(ppe, EN75_PPE_FLOW_CFG, val);

	/* Airoha vendor sets this alongside FLOW_CFG (PpeParseLayerInfo). */
	ppe_w32(ppe, EN75_PPE_PARSE_LAYER_INFO, EN75_PPE_PARSE_LAYER_INFO_VAL);

	val = FIELD_PREP(EN75_PPE_UNBIND_AGE_MIN_PACKETS, 1000) |
	      FIELD_PREP(EN75_PPE_UNBIND_AGE_DELTA, 3);
	ppe_w32(ppe, EN75_PPE_UNBIND_AGE, val);

	val = FIELD_PREP(EN75_PPE_BIND_AGE0_DELTA_UDP, 12) |
	      FIELD_PREP(EN75_PPE_BIND_AGE0_DELTA_NON_L4, 1);
	ppe_w32(ppe, EN75_PPE_BIND_AGE0, val);

	val = FIELD_PREP(EN75_PPE_BIND_AGE1_DELTA_TCP_FIN, 1) |
	      FIELD_PREP(EN75_PPE_BIND_AGE1_DELTA_TCP, 7);
	ppe_w32(ppe, EN75_PPE_BIND_AGE1, val);

	val = EN75_PPE_BIND_LIMIT0_QUARTER | EN75_PPE_BIND_LIMIT0_HALF;
	ppe_w32(ppe, EN75_PPE_BIND_LIMIT0, val);

	val = EN75_PPE_BIND_LIMIT1_FULL |
	      FIELD_PREP(EN75_PPE_BIND_LIMIT1_NON_L4, 1);
	ppe_w32(ppe, EN75_PPE_BIND_LIMIT1, val);

	val = FIELD_PREP(EN75_PPE_BIND_RATE_BIND, 30) |
	      FIELD_PREP(EN75_PPE_BIND_RATE_PREBIND, 1);
	ppe_w32(ppe, EN75_PPE_BIND_RATE, val);

	val = EN75_PPE_GLO_CFG_EN |
	      EN75_PPE_GLO_CFG_IP4_L4_CS_DROP |
	      EN75_PPE_GLO_CFG_IP4_CS_DROP |
	      EN75_PPE_GLO_CFG_FLOW_DROP_UPDATE;
	ppe_w32(ppe, EN75_PPE_GLO_CFG, val);

	/*
	 * Default CPU port for punted (unbound / miss) traffic. Vendor writes
	 * 0x500 here from three separate code paths (SetGdmaFwd / force-port);
	 * we previously wrote 0, which also selected the CPU but did not match
	 * the vendor's PSE egress port encoding used when a bound flow hits.
	 */
	ppe_w32(ppe, EN75_PPE_DEFAULT_CPU_PORT, 0x500);

	ppe_wait_busy(ppe);

	dev_info(ppe->dev,
		 "PPE enabled: %u FOE entries @ %pad, GLO_CFG=%#x TB_CFG=%#x\n",
		 EN75_PPE_ENTRIES, &ppe->foe_phys,
		 ppe_r32(ppe, EN75_PPE_GLO_CFG),
		 ppe_r32(ppe, EN75_PPE_TB_CFG));
}

void en75_ppe_stop(struct en75_ppe *ppe)
{
	if (IS_ERR_OR_NULL(ppe))
		return;

	en75_ppe_offload_deinit(ppe);

	debugfs_remove_recursive(ppe->debugfs);
	ppe->debugfs = NULL;

	ppe_w32(ppe, EN75_PPE_CACHE_EN,
		ppe_r32(ppe, EN75_PPE_CACHE_EN) & ~EN75_PPE_CACHE_EN_EBL);
	ppe_w32(ppe, EN75_PPE_GLO_CFG, 0);
	ppe_wait_busy(ppe);
}
