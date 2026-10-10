/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Packet Processing Engine (PPE) for the EcoNet EN751221.
 *
 * The EN751221 PPE is register- and FOE-entry-compatible with the MediaTek
 * "netsys v1" PPE (mt7621 class): 80-byte FOE entries, 16384 entries, and the
 * same register block laid out at offset 0x200 within the PPE window that
 * econet_eth.c maps as en751221_regs.ppe (0xBFB50C00). The accessors and
 * register definitions below mirror the mainline mtk_ppe driver but are kept
 * native to this driver to avoid pulling in the struct mtk_eth coupling.
 *
 * Milestone 1: bring the engine up against an empty FOE table. With no bound
 * entries and the GDM forwarding left untouched, no packet is ever steered to
 * the PPE, so the datapath is unchanged; this only validates register access,
 * FOE table allocation and engine enable.
 */
#ifndef ECONET_PPE_H
#define ECONET_PPE_H

#include <linux/bitfield.h>
#include <linux/rhashtable.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct device;
struct dentry;

/* 1024 << 4 = 16384 entries, matching the vendor gPpeFoeNum. */
#define EN75_PPE_ENTRIES_SHIFT		4
#define EN75_PPE_ENTRIES		(1024 << EN75_PPE_ENTRIES_SHIFT)

/* Register offsets, relative to the start of the ppe[] window. */
#define EN75_PPE_GLO_CFG		0x200
#define EN75_PPE_GLO_CFG_EN			BIT(0)
#define EN75_PPE_GLO_CFG_IP4_L4_CS_DROP		BIT(2)
#define EN75_PPE_GLO_CFG_IP4_CS_DROP		BIT(3)
#define EN75_PPE_GLO_CFG_FLOW_DROP_UPDATE	BIT(9)
#define EN75_PPE_GLO_CFG_BUSY			BIT(31)

#define EN75_PPE_FLOW_CFG		0x204
#define EN75_PPE_FLOW_CFG_IP4_TCP_FRAG		BIT(6)
#define EN75_PPE_FLOW_CFG_IP4_UDP_FRAG		BIT(7)
#define EN75_PPE_FLOW_CFG_IP6_3T_ROUTE		BIT(8)
#define EN75_PPE_FLOW_CFG_IP6_5T_ROUTE		BIT(9)
#define EN75_PPE_FLOW_CFG_IP6_6RD		BIT(10)
#define EN75_PPE_FLOW_CFG_IP4_NAT		BIT(12)
#define EN75_PPE_FLOW_CFG_IP4_NAPT		BIT(13)
#define EN75_PPE_FLOW_CFG_IP4_DSLITE		BIT(14)
/*
 * Vendor (Airoha hw_nat) sets bit15 and bit16 in every FLOW_CFG write and in
 * PpeSetFoeEbl (which masks the register down to *only* bit16 before writing,
 * proving bit16 is the FOE-lookup / bind-hit "engine enable"). Mainline mtk
 * never sets these because its netsys enables the engine differently; on
 * EN751221 the search+punt path works without them but bound entries are never
 * hit/forwarded until both are set. bit15 is always set alongside bit16.
 */
#define EN75_PPE_FLOW_CFG_UDP_IP4		BIT(15)
#define EN75_PPE_FLOW_CFG_FOE_EBL		BIT(16)
#define EN75_PPE_FLOW_CFG_IP4_NAT_FRAG		BIT(17)

#define EN75_PPE_IP_PROTO_CHK		0x208
#define EN75_PPE_IP_PROTO_CHK_IPV4		GENMASK(15, 0)
#define EN75_PPE_IP_PROTO_CHK_IPV6		GENMASK(31, 16)

/*
 * Airoha-only "parse layer info" register (no mainline mtk equivalent). The
 * vendor hw_nat writes it in PpeParseLayerInfo immediately after FLOW_CFG and
 * it is the only PPE-block core-init register the vendor sets that our driver
 * otherwise leaves at reset 0. Value taken verbatim from the vendor blob; its
 * four bytes (04 2a 11 06) read as per-layer-type enables: IPv4 encap, IPv6
 * encap, UDP, TCP.
 */
#define EN75_PPE_PARSE_LAYER_INFO	0x20c
#define EN75_PPE_PARSE_LAYER_INFO_VAL	0x042a1106

#define EN75_PPE_TB_CFG			0x21c
#define EN75_PPE_TB_CFG_ENTRY_NUM		GENMASK(2, 0)
#define EN75_PPE_TB_CFG_ENTRY_80B		BIT(3)
#define EN75_PPE_TB_CFG_SEARCH_MISS		GENMASK(5, 4)
#define EN75_PPE_TB_CFG_AGE_NON_L4		BIT(7)
#define EN75_PPE_TB_CFG_AGE_UNBIND		BIT(8)
#define EN75_PPE_TB_CFG_AGE_TCP			BIT(9)
#define EN75_PPE_TB_CFG_AGE_UDP			BIT(10)
#define EN75_PPE_TB_CFG_AGE_TCP_FIN		BIT(11)
#define EN75_PPE_TB_CFG_KEEPALIVE		GENMASK(13, 12)
#define EN75_PPE_TB_CFG_HASH_MODE		GENMASK(15, 14)
#define EN75_PPE_TB_CFG_SCAN_MODE		GENMASK(17, 16)

/* Field values (see mainline mtk_ppe_regs.h enums). */
#define EN75_PPE_SCAN_MODE_CHECK_AGE		1
#define EN75_PPE_KEEPALIVE_DISABLE		0
#define EN75_PPE_SEARCH_MISS_ACTION_FORWARD_BUILD	3
#define EN75_PPE_SEARCH_MISS_ACTION_FORWARD	2

#define EN75_PPE_TB_USED		0x224

#define EN75_PPE_TB_BASE		0x220

#define EN75_PPE_BIND_RATE		0x228
#define EN75_PPE_BIND_RATE_BIND			GENMASK(15, 0)
#define EN75_PPE_BIND_RATE_PREBIND		GENMASK(31, 16)

#define EN75_PPE_BIND_LIMIT0		0x22c
#define EN75_PPE_BIND_LIMIT0_QUARTER		GENMASK(13, 0)
#define EN75_PPE_BIND_LIMIT0_HALF		GENMASK(29, 16)

#define EN75_PPE_BIND_LIMIT1		0x230
#define EN75_PPE_BIND_LIMIT1_FULL		GENMASK(13, 0)
#define EN75_PPE_BIND_LIMIT1_NON_L4		GENMASK(23, 16)

#define EN75_PPE_KEEPALIVE		0x234

#define EN75_PPE_UNBIND_AGE		0x238
#define EN75_PPE_UNBIND_AGE_MIN_PACKETS		GENMASK(31, 16)
#define EN75_PPE_UNBIND_AGE_DELTA		GENMASK(7, 0)

#define EN75_PPE_BIND_AGE0		0x23c
#define EN75_PPE_BIND_AGE0_DELTA_NON_L4		GENMASK(30, 16)
#define EN75_PPE_BIND_AGE0_DELTA_UDP		GENMASK(14, 0)

#define EN75_PPE_BIND_AGE1		0x240
#define EN75_PPE_BIND_AGE1_DELTA_TCP_FIN	GENMASK(30, 16)
#define EN75_PPE_BIND_AGE1_DELTA_TCP		GENMASK(14, 0)

#define EN75_PPE_DEFAULT_CPU_PORT	0x248

/* 0x320 is a cache COMMAND register on EN751221 (vendor ppeCacheCmd_request):
 * write an op with the REQ bit set, hardware clears REQ when done. 0x334 is
 * the cache enable/config register; vendor PpeSetCacheEbl writes magic 0x33
 * (enable + mode bits) for chip families 0x0007/0x0008.
 */
#define EN75_PPE_CACHE_CTL		0x320
#define EN75_PPE_CACHE_CTL_REQ			BIT(8)
#define EN75_PPE_CACHE_CTL_CLEAR_ALL		0x4100
#define EN75_PPE_CACHE_EN		0x334
#define EN75_PPE_CACHE_EN_EBL			BIT(0)
#define EN75_PPE_CACHE_EN_MAGIC			0x33

/* Frame-engine free-running timestamp (FE+0x10), drives FOE aging. */
#define EN75_FE_FOE_TS			0x10

/* PSE port numbers used as the FOE egress (ib2 DEST_PORT). */
#define EN75_PPE_PSE_CPU_PORT		0
#define EN75_PPE_PSE_GDM1_PORT		1
#define EN75_PPE_PSE_GDM2_PORT		2

#include "econet_foe.h"

/** Per-PPE runtime state. */
struct flow_cls_offload;

/* Bring-up diagnostic counters, dumped by debugfs econet_ppe/diag. Plain
 * u32s: increments race harmlessly, this is inspection-only. */
enum en75_diag_id {
	EN75_DIAG_RXC_CALL,
	EN75_DIAG_RXC_NOPARSE,
	EN75_DIAG_RXC_NOLK4,
	EN75_DIAG_RXC_NOLK6,
	EN75_DIAG_RXC_NOTPEND,
	EN75_DIAG_RXC_BACKOFF,
	EN75_DIAG_RXC_NOSLOT4,
	EN75_DIAG_RXC_NOSLOT6,
	EN75_DIAG_RXC_BOUND4,
	EN75_DIAG_RXC_BOUND6,
	EN75_DIAG_REPL_OK4,
	EN75_DIAG_REPL_OK6,
	EN75_DIAG_REPL_REJ_BASIC,
	EN75_DIAG_REPL_REJ_ACT,
	EN75_DIAG_REPL_REJ_ODEV,
	EN75_DIAG_REPL_REJ_MAC,
	EN75_DIAG_REPL_REJ_PORTS,
	EN75_DIAG_COUNT,
};
extern u32 en75_diag[EN75_DIAG_COUNT];
extern u32 en75_diag_crsn[32];

struct en75_ppe {
	struct device		*dev;
	/* GDM1's netdev: the only egress the FOE can forward to. */
	struct net_device	*ndev;
	void __iomem		*base;
	/* frame-engine timestamp register window, for FOE aging stamps. */
	void __iomem		*fe;
	struct en75_foe_entry	*foe;
	dma_addr_t		foe_phys;
	struct dentry		*debugfs;
	/* hash slot of the debug test-bind entry, or ~0u if none. */
	u32			test_hash;
	/* cookie -> en75_flow_entry map for offloaded flows. */
	struct rhashtable	flow_table;
	bool			flow_table_ready;
	/* devices offering indirect (bridge/VLAN) flow blocks. */
	struct list_head	indr_block_list;
	/* serialises FOE table/cache writes between process and RX softirq. */
	spinlock_t		foe_lock;
	/* tuple -> en75_flow_entry map, drives the RX-hint commit. */
	struct rhashtable	tuple_table;
	bool			tuple_table_ready;
};

/**
 * en75_ppe_init() - allocate the FOE table and the PPE state.
 * @dev:  device used for the coherent DMA allocation (devm managed).
 * @base: ioremapped base of the ppe[] register window.
 * @fe:   ioremapped base of the frame-engine register window (for timestamps).
 *
 * Returns the PPE state on success or an ERR_PTR() on failure. The engine is
 * not started here; call en75_ppe_start() afterwards.
 */
struct en75_ppe *en75_ppe_init(struct device *dev, void __iomem *base,
			       void __iomem *fe);

/** en75_ppe_start() - program the registers and enable the engine. */
void en75_ppe_start(struct en75_ppe *ppe);

/** en75_ppe_stop() - disable the engine (safe to call before module unload). */
void en75_ppe_stop(struct en75_ppe *ppe);

/**
 * en75_ppe_foe_commit() - publish a built FOE entry into the live table.
 * @hash:  table index from en75_foe_entry_hash().
 * @entry: fully built entry; its ib1 (state/static/timestamp) must be final.
 *
 * Writes the entry body before the information block so the engine never
 * observes a half-written BIND entry, then flushes the FOE cache.
 */
void en75_ppe_foe_commit(struct en75_ppe *ppe, u32 hash,
			 struct en75_foe_entry *entry);

/** en75_ppe_foe_clear() - invalidate the live FOE entry at @hash. */
void en75_ppe_foe_clear(struct en75_ppe *ppe, u32 hash);

/*
 * No-lock variants: the caller must hold ppe->foe_lock. Used by the offload
 * code so the FOE write and the owning flow-entry state change are atomic
 * against a concurrent RX-hint commit or teardown.
 */
void __en75_ppe_foe_commit(struct en75_ppe *ppe, u32 hash,
			   struct en75_foe_entry *entry);
void __en75_ppe_foe_clear(struct en75_ppe *ppe, u32 hash);

/** en75_ppe_timestamp() - current frame-engine timestamp (BIND_TIMESTAMP bits). */
u16 en75_ppe_timestamp(struct en75_ppe *ppe);

/* nf_flowtable / TC flower offload (econet_ppe_offload.c). */
int en75_ppe_offload_init(struct en75_ppe *ppe);
void en75_ppe_offload_deinit(struct en75_ppe *ppe);
int en75_ppe_offload_cmd(struct en75_ppe *ppe, struct flow_cls_offload *cls);

/**
 * en75_ppe_offload_rx_commit() - bind a pending offloaded flow using the slot
 * the hardware reported for a missed packet.
 * @skb:     the received packet (UN_HIT); its ingress 5-tuple is the key.
 * @hw_slot: the FOE slot the engine computed, from the RX descriptor.
 *
 * Called from the RX softirq. If a flow with this tuple was staged by a TC/
 * flowtable REPLACE but not yet committed, its prebuilt entry is published at
 * @hw_slot - the exact slot the engine will search - so subsequent packets hit.
 */
struct sk_buff;
void en75_ppe_offload_rx_commit(struct en75_ppe *ppe, struct sk_buff *skb,
				u32 hw_slot);

#endif /* ECONET_PPE_H */
