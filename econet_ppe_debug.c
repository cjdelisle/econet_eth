// SPDX-License-Identifier: GPL-2.0-only
/*
 * PPE/FOE debugfs introspection and test-bind facility, split out of
 * econet_ppe.c so the functional bring-up code isn't interleaved with
 * debug-only dumping (matches the existing split between econet_eth.c and
 * econet_eth_debug.c for the QDMA side).
 */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "econet_eth.h"
#include "econet_ppe.h"

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
			en75_ppe_foe_clear(ppe, ppe->test_hash);
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

/**
 * en75_ppe_debugfs_init() - create the econet_ppe/ debugfs directory.
 *
 * Always built in, like the rest of this driver's debugfs support (see
 * econet_eth_debug.c) -- there is no Kconfig symbol gating it, debugfs's own
 * CONFIG_DEBUG_FS already governs whether any of it is reachable.
 */
void en75_ppe_debugfs_init(struct en75_ppe *ppe)
{
	ppe->debugfs = debugfs_create_dir("econet_ppe", NULL);
	debugfs_create_file("foe", 0444, ppe->debugfs, ppe, &en75_ppe_foe_fops);
	debugfs_create_file("test_bind", 0200, ppe->debugfs, ppe,
			    &en75_ppe_test_bind_fops);
	debugfs_create_file("foe_slot", 0644, ppe->debugfs, ppe,
			    &en75_ppe_foe_slot_fops);
}
