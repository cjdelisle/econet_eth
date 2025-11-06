// SPDX-License-Identifier: GPL-2.0-only

#ifndef ECONET_ETH_H
#define ECONET_ETH_H

#include <linux/types.h>

#include "qdma_desc.h"
#include "econet_eth_regs.h"

struct en75_debug;

#define NUM_QDMA 2
#define NUM_QDMA_CHAINS 2

struct en75_debug_qdma_chain_conf {
	struct qdma_desc *rx_descs;
	int rx_count;
	struct qdma_desc *tx_descs;
	int tx_count;
};

struct en75_debug_qdma_conf {
	struct qregs __iomem *regs;
	struct en75_debug_qdma_chain_conf chains[NUM_QDMA_CHAINS];
};

struct en75_debug_conf {
	struct en75_debug_qdma_conf qdma[NUM_QDMA];
};

struct en75_debug *en75_debugfs_init(struct en75_debug_conf *config);
void en75_debugfs_exit(struct en75_debug *debug);

#endif