// SPDX-License-Identifier: GPL-2.0-only
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "econet_eth.h"

struct en75_qdma_chain_debug {
	struct dentry *descs;
	struct en75_debug_qdma_chain_conf *config;
	struct en75_qdma_debug *qdma;
	int chain_n;
};

struct en75_qdma_debug {
	struct dentry *dir;
	struct en75_debug_qdma_conf *config;
	struct en75_qdma_chain_debug chains[NUM_QDMA_CHAINS];
};

struct en75_debug {
	struct dentry *dir;
	struct en75_qdma_debug qdma[NUM_QDMA];
	struct en75_debug_conf config;
};

static void print_erx(struct seq_file *m, struct qdma_desc_erx *erx)
{
	seq_printf(m, "crsn=%d sport=%d ppe=%d"
		"%s%s%s%s%s%s%s",
		get_erx_crsn(erx), get_erx_sport(erx), get_erx_ppe_entry(erx),

		is_erx_ip6(erx) ? " IP6" : "",
		is_erx_ip4(erx) ? " IP4" : "",
		is_erx_ip4f(erx) ? " IP4F" : "",
		is_erx_tack(erx) ? " TACK" : "",
		is_erx_l2vld(erx) ? " L2VLD" : "",
		is_erx_l4f(erx) ? " L4F" : "",
		is_erx_untag(erx) ? " UNTAG" : ""
	);

	if (erx->sp_tag) {
		seq_printf(m, " sp_tag=%.4x", erx->sp_tag);
	}
	if (erx->tci) {
		seq_printf(m, " tci=%.4x", erx->tci);
	}
	if (erx->unknown0) {
		seq_printf(m, " unknown0=%.8x", erx->unknown0);
	}
	if (get_erx_unknown1(erx)) {
		seq_printf(m, " unknown1=%.2x", get_erx_unknown1(erx));
	}
	if (get_erx_unknown2(erx)) {
		seq_printf(m, " unknown2=%.8x", get_erx_unknown2(erx));
	}
}

static void print_etx(struct seq_file *m, struct qdma_desc_etx *etx)
{
	seq_printf(m, "fport=%d"
		"%s%s%s%s%s",
		get_etx_fport(etx),

		is_etx_oam(etx) ? " OAM" : "",
		is_etx_ico(etx) ? " ICO" : "",
		is_etx_sco(etx) ? " SCO" : "",
		is_etx_tco(etx) ? " TCO" : "",
		is_etx_uco(etx) ? " UCO" : ""
	);

	if (get_etx_channel(etx)) {
		seq_printf(m, " channel=%d", get_etx_channel(etx));
	}
	if (get_etx_queue(etx)) {
		seq_printf(m, " queue=%d", get_etx_queue(etx));
	}
	if (get_etx_sp_tag(etx)) {
		seq_printf(m, " sp_tag=%.4x", get_etx_sp_tag(etx));
	}
	if (get_etx_udf_pmap(etx)) {
		seq_printf(m, " udf_pmap=%.2x", get_etx_udf_pmap(etx));
	}
	if (is_etx_vlan_en(etx)) {
		seq_printf(m, " vlan_type=%.2x", get_etx_vlan_type(etx));
	}
	if (etx->vlan_tag) {
		seq_printf(m, " vlan_tag=%.4x", etx->vlan_tag);
	}
}

static void print_desc(struct seq_file *m, struct qdma_desc *desc) {
	seq_printf(m, "len=%d\taddr=%.8x next=%d%s%s%s",
		desc->pkt_len,
		desc->pkt_addr,
		get_desc_next_idx(desc),
		is_desc_done(desc) ? " DONE" : "",
		is_desc_dropped(desc) ? " DROPPED" : "",
		is_desc_nls(desc) ? " NLS" : ""
	);
	if (desc->unknown0) {
		seq_printf(m, " unknown0=%.8x", desc->unknown0);
	}
	if (get_desc_unknown1(desc)) {
		seq_printf(m, " unknown1=%.4x", get_desc_unknown1(desc));
	}
	if (get_desc_unknown2(desc)) {
		seq_printf(m, " unknown2=%.4x", get_desc_unknown2(desc));
	}
}

static int en75_qdma_descs(struct seq_file *m, void *v)
{
	struct en75_qdma_chain_debug *chain = m->private;
	struct qchain_regs __iomem *qchain_reg;
	int i;

	qchain_reg = chain->chain_n == 1 ?
		&chain->qdma->config->regs->qchain1 :
		&chain->qdma->config->regs->qchain0;

	seq_printf(m, "QDMA RX Descriptors driver_idx=%d hardware_idx=%d\n",
		readl(&qchain_reg->rx_cpui), readl(&qchain_reg->rx_hwi));
	for (i = 0; i < chain->config->rx_count; i++) {
		struct qdma_desc *desc = &chain->config->rx_descs[i];
		seq_printf(m, "  %d ", i);
		print_desc(m, desc);
		seq_puts(m, " ");
		print_erx(m, &desc->t.erx);
		seq_puts(m, "\n");
	}

	seq_printf(m, "QDMA TX Descriptors driver_idx=%d hardware_idx=%d\n",
		readl(&qchain_reg->tx_cpui), readl(&qchain_reg->tx_hwi));
	for (i = 0; i < chain->config->tx_count; i++) {
		struct qdma_desc *desc = &chain->config->tx_descs[i];
		seq_printf(m, "  %d ", i);
		print_desc(m, desc);
		seq_puts(m, " ");
		print_etx(m, &desc->t.etx);
		seq_puts(m, "\n");
	}

	return 0;
}

static int en75_descs_open(struct inode *inode, struct file *file)
{
	return single_open(file, en75_qdma_descs, inode->i_private);
}

static const struct file_operations en75_descs_fops = {
	.owner   = THIS_MODULE,
	.open    = en75_descs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int en75_init_qdma(struct en75_qdma_debug *qdma_debug)
{
	struct en75_debug_qdma_conf *config = qdma_debug->config;

	BUILD_BUG_ON(ARRAY_SIZE(qdma_debug->chains) != ARRAY_SIZE(config->chains));
	for (int i = 0; i < ARRAY_SIZE(qdma_debug->chains); i++) {
		char filename[8] = {0};

		if (config->chains[i].rx_descs == NULL ||
		    config->chains[i].tx_descs == NULL)
			continue;

		snprintf(filename, sizeof(filename) - 1, "descs%d", i);
		qdma_debug->chains[i].descs =
			debugfs_create_file(filename, 0444,
					    qdma_debug->dir,
					    &qdma_debug->chains[i],
					    &en75_descs_fops);

		if (!qdma_debug->chains[i].descs)
			return -ENOMEM;

		qdma_debug->chains[i].config = &config->chains[i];
		qdma_debug->chains[i].qdma = qdma_debug;
		qdma_debug->chains[i].chain_n = i;
	}

	return 0;
}

/* Call from your module init */
struct en75_debug *en75_debugfs_init(struct en75_debug_conf *config)
{
	struct en75_debug *debug;
	int ret = -EINVAL;

	debug = kzalloc(sizeof(*debug), GFP_KERNEL);
	if (!debug)
		return ERR_PTR(-ENOMEM);

	memcpy(&debug->config, config, sizeof(debug->config));
	config = &debug->config;

	debug->dir = debugfs_create_dir("econet_eth", NULL);
	if (!debug->dir) {
		ret = -ENOMEM;
		goto err_dir;
	}

	BUILD_BUG_ON(ARRAY_SIZE(debug->qdma) != ARRAY_SIZE(config->qdma));
	for (int i = 0; i < ARRAY_SIZE(debug->qdma); i++) {
		char qdma_dirname[8] = {0};

		if (config->qdma[i].regs == NULL)
			continue;

		snprintf(qdma_dirname, sizeof(qdma_dirname) - 1, "qdma%d", i);
		debug->qdma[i].dir =
			debugfs_create_dir(qdma_dirname, debug->dir);
		if (!debug->qdma[i].dir) {
			ret = -ENOMEM;
			goto err_file;
		}

		debug->qdma[i].config = &config->qdma[i];
		ret = en75_init_qdma(&debug->qdma[i]);
		if (ret)
			goto err_file;
	}

	return debug;

err_file:
	debugfs_remove_recursive(debug->dir);
err_dir:
	kfree(debug);
	return ERR_PTR(ret);
}

/* Call from your module exit */
void en75_debugfs_exit(struct en75_debug *debug)
{
	if (!debug) 
		return;
	debugfs_remove_recursive(debug->dir);
	kfree(debug);
}