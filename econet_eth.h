// SPDX-License-Identifier: GPL-2.0-only
#ifndef ECONET_ETH_H
#define ECONET_ETH_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>

#include "qdma_desc.h"
#include "qdma_regs.h"

struct en75_debug;

#define NUM_QDMA 2
#define NUM_QDMA_CHAINS 2
#define EN75_QDMA_IRQS 1
#define AIROHA_MAX_PACKET_SIZE		2048

#define EN75_NUM_QUEUES 8

struct en75_debug_qdma_chain_conf {
	struct desc *rx_descs;
	int rx_count;
	struct desc *tx_descs;
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

enum en75_fport {
	DPORT_CPU		= 0,
	DPORT_GDMA1		= 1,
	DPORT_GDMA2		= 2,
	DPORT_UNKNOWN_3		= 3,
	DPORT_PPE		= 4,
	DPORT_QDMA		= 5,
	DPORT_QDMA_HW		= 6,
	DPORT_DISCARD		= 7,
};

struct en75_eth;

/* Called in softirq context */
int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb, int sport);

#define AIROHA_NUM_TX_RING		2
#define AIROHA_NUM_RX_RING		2
#define AIROHA_NUM_TX_DONE		1

struct en75_qdma_cfg {
	int num_rx_descs[AIROHA_NUM_RX_RING];
	int num_tx_descs[AIROHA_NUM_TX_RING];
	int done_list_size[AIROHA_NUM_TX_DONE];
	int num_fwd_descs;
	int fwd_max_packet_size;
	int fwd_low_threshold;
};

struct en75_qdma;

struct en75_qdma *en75_qdma_new(struct platform_device *pdev,
				void __iomem *qdma_regs,
				int id,
				int *irqs,
				int num_irqs,
				struct en75_qdma_cfg *cfg,
				struct en75_eth *eth);

int en75_qdma_use(struct en75_qdma *qdma);
int en75_qdma_unuse(struct en75_qdma *qdma);
int en75_qdma_destroy(struct en75_qdma *qdma);
int en75_qdma_xmit(struct en75_qdma *qdma, struct sk_buff *skb,
		   union desc_msg *msg, int qid);

#define en75_rreg(reg) __extension__({ \
		BUILD_BUG_ON(sizeof(*(reg)) != sizeof(u32)); \
		union { typeof(*(reg)) v; u32 w; } __r = { .w = readl(reg) }; \
		__r.v; \
	})

#define en75_wreg(val, reg) do { \
		BUILD_BUG_ON(sizeof(*(reg)) != sizeof(u32)); \
		BUILD_BUG_ON(!__same_type(*(reg), (val))); \
		union { typeof(*(reg)) v; u32 w; } __w = { .v = (val) }; \
		writel(__w.w, (reg)); \
	} while (0)
#endif