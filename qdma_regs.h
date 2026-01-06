// SPDX-License-Identifier: GPL-2.0-only
#ifndef QDMA_REGS_H
#define QDMA_REGS_H

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/types.h>

#ifndef FIELD_SET
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))
#endif

/** qchain_regs: QDMA Chain Registers */
struct qchain_regs {
	/** qchain_regs_txbase: TX descriptor array address */
	dma_addr_t txbase;

	/** qchain_regs_rxbase: RX descriptor array address */
	dma_addr_t rxbase;

	/** qchain_regs_tx_cpui: TX ring CPU (driver) index */
	u32 tx_cpui;

	/** qchain_regs_tx_hwi: TX ring hardware index */
	u32 tx_hwi;

	/** qchain_regs_rx_cpui: RX ring CPU (driver) index */
	u32 rx_cpui;

	/** qchain_regs_rx_hwi: TX ring hardware index */
	u32 rx_hwi;

};

/** qregs: QDMA Registers */
struct qregs {
	/** qregs_version:  */
	u32 version;

	/** qregs_qcfg:  */
	struct qregs_qcfg {
		/**
		 * See accessors:
		 * is_qregs_qcfg_rx_2b_offset()
		 * set_qregs_qcfg_rx_2b_offset()
		 * get_qregs_qcfg_dma_pref()
		 * set_qregs_qcfg_dma_pref()
		 * is_qregs_qcfg_msg_word_swap()
		 * set_qregs_qcfg_msg_word_swap()
		 * is_qregs_qcfg_dscp_byte_swap()
		 * set_qregs_qcfg_dscp_byte_swap()
		 * is_qregs_qcfg_payload_byte_sw()
		 * set_qregs_qcfg_payload_byte_sw()
		 * is_qregs_qcfg_vchnl_map_en()
		 * set_qregs_qcfg_vchnl_map_en()
		 * is_qregs_qcfg_vchnl_map_mode()
		 * set_qregs_qcfg_vchnl_map_mode()
		 * is_qregs_qcfg_qdma_lpbk_rxq_sel()
		 * set_qregs_qcfg_qdma_lpbk_rxq_sel()
		 * is_qregs_qcfg_slm_release_en()
		 * set_qregs_qcfg_slm_release_en()
		 * is_qregs_qcfg_tx_immediate_done()
		 * set_qregs_qcfg_tx_immediate_done()
		 * is_qregs_qcfg_irq_en()
		 * set_qregs_qcfg_irq_en()
		 * is_qregs_qcfg_gdm_loopback()
		 * set_qregs_qcfg_gdm_loopback()
		 * is_qregs_qcfg_qdma_loopback()
		 * set_qregs_qcfg_qdma_loopback()
		 * is_qregs_qcfg_check_done()
		 * set_qregs_qcfg_check_done()
		 * is_qregs_qcfg_tx_wb_done()
		 * set_qregs_qcfg_tx_wb_done()
		 * get_qregs_qcfg_burst_size()
		 * set_qregs_qcfg_burst_size()
		 * is_qregs_qcfg_rx_dma_busy()
		 * set_qregs_qcfg_rx_dma_busy()
		 * is_qregs_qcfg_rx_dma_en()
		 * set_qregs_qcfg_rx_dma_en()
		 * is_qregs_qcfg_tx_dma_busy()
		 * set_qregs_qcfg_tx_dma_busy()
		 * is_qregs_qcfg_tx_dma_en()
		 * set_qregs_qcfg_tx_dma_en()
		 */
		u32 bitfield_0;

	} qdma_cfg;

	/** qregs_qchain0:  */
	struct qchain_regs qchain0;

	/**
	 * qregs_hwf_desc_addr: Hardware forwarding descriptor table address. This
	 * memory must be forward-descriptor-size (16 bytes) * fwd_desc_n in size.
	 */
	dma_addr_t hwf_desc_addr;

	/**
	 * qregs_hwf_data_addr: Hardware forwarding packet content address. This memory
	 * must be pkt_sz *  fwd_desc_n in size.
	 */
	dma_addr_t hwf_data_addr;

	/**
	 * See accessors:
	 * get_qregs_hwf_cfg_pkt_sz()
	 * set_qregs_hwf_cfg_pkt_sz()
	 * get_qregs_hwf_cfg_low_th()
	 * set_qregs_hwf_cfg_low_th()
	 */
	struct hwf_cfg { u32 word; } hwf_cfg;

	/** qregs_hwf_cfg1: Hardware forwarding configuration (also called LMGR) */
	struct qregs_hwf_cfg1 {
		/**
		 * See accessors:
		 * is_qregs_hwf_cfg1_start()
		 * set_qregs_hwf_cfg1_start()
		 * is_qregs_hwf_cfg1_overhead_en()
		 * set_qregs_hwf_cfg1_overhead_en()
		 */
		u8 bitfield_0;

		/**
		 * qregs_hwf_cfg1_overhead: Amount of overhead to add to packet size for
		 * accounting
		 */
		u8 overhead;

		/** qregs_hwf_cfg1_fwd_desc_n: Number of forward descriptors to use. */
		u16 fwd_desc_n;

	} hwf_cfg1;

	/**
	 * qregs_int_status: When an interrupt is triggered, these are the pending
	 * events
	 */
	u32 int_status;

	/** qregs_int_enable: Enabled interrupts */
	u32 int_enable;

	u8 unused_0[32];

	/** qregs_tx_int_delay: Interrupt delay for reducing interrupt load */
	u32 tx_int_delay;

	/** qregs_rx_int_delay: Interrupt delay for reducing interrupt load */
	u32 rx_int_delay;

	/** qregs_doneq:  */
	struct qregs_doneq {
		/** qregs_doneq_addr:  */
		dma_addr_t address;

		/** qregs_doneq_cfg:  */
		struct qregs_doneq_cfg {
			/**
			 * qregs_doneq_cfg_intt: When done queue is this full, fire an interrupt, max
			 * 4095
			 */
			u16 int_threshold;

			/**
			 * qregs_doneq_cfg_sz: Size of the done queue buffer in 4 byte units, max
			 * 4095
			 */
			u16 size;

		} config;

		/**
		 * qregs_doneq_pop_back: Pop this number of items from the back of the the
		 * done queue, max 255
		 */
		u32 pop_back;

		/** qregs_doneq_state:  */
		struct qregs_doneq_state {
			/** qregs_doneq_state_len: Number of items waiting in the queue */
			u16 length;

			/** qregs_doneq_state_head: Index of the first item in the list */
			u16 head_index;

		} state;

		/**
		 * qregs_doneq_wait_time: If there is anything in the queue, fire an interrupt
		 * after this number of units of time, unit is 20 microseconds.
		 */
		u32 wait_time;

	} done_queue;

	/** qregs_tx_congest_cfg:  */
	struct qregs_tx_congest_cfg {
		/**
		 * See accessors:
		 * is_qregs_tx_congest_cfg_tail_drop_en()
		 * set_qregs_tx_congest_cfg_tail_drop_en()
		 * is_qregs_tx_congest_cfg_dei_drop_en()
		 * set_qregs_tx_congest_cfg_dei_drop_en()
		 * is_qregs_tx_congest_cfg_dyncong_en()
		 * set_qregs_tx_congest_cfg_dyncong_en()
		 * is_qregs_tx_congest_cfg_max_thr_blk_tx1()
		 * set_qregs_tx_congest_cfg_max_thr_blk_tx1()
		 * is_qregs_tx_congest_cfg_min_thr_blk_tx1()
		 * set_qregs_tx_congest_cfg_min_thr_blk_tx1()
		 * is_qregs_tx_congest_cfg_max_thr_blk_tx0()
		 * set_qregs_tx_congest_cfg_max_thr_blk_tx0()
		 * is_qregs_tx_congest_cfg_min_thr_blk_tx0()
		 * set_qregs_tx_congest_cfg_min_thr_blk_tx0()
		 * get_qregs_tx_congest_cfg_dyncong_margin()
		 * set_qregs_tx_congest_cfg_dyncong_margin()
		 * get_qregs_tx_congest_cfg_dyncong_dei_scale()
		 * set_qregs_tx_congest_cfg_dyncong_dei_scale()
		 * is_qregs_tx_congest_cfg_dyncong_upd_wrr()
		 * set_qregs_tx_congest_cfg_dyncong_upd_wrr()
		 * is_qregs_tx_congest_cfg_dyncong_upd_txrx()
		 * set_qregs_tx_congest_cfg_dyncong_upd_txrx()
		 * is_qregs_tx_congest_cfg_dyncong_upd_tick()
		 * set_qregs_tx_congest_cfg_dyncong_upd_tick()
		 */
		u16 bitfield_0;

		/**
		 * qregs_tx_congest_cfg_dyncong_tick: Dynamic congestion ticker rate in
		 * microseconds
		 */
		u16 dyncong_tick;

	} tx_congest_cfg;

	/** qregs_tx_congest_thr:  */
	struct qregs_tx_congest_thr {
		/**
		 * qregs_tx_congest_thr_max: When total buffer usage exceeds this, drop all
		 * packets except VIP
		 */
		u16 max;

		/**
		 * qregs_tx_congest_thr_min: When total buffer usage exceeds this, dynamic
		 * congestion control will start
		 */
		u16 min;

	} tx_congest_thr;

	u8 unused_1[132];

	/** qregs_rxring_size:  */
	struct qregs_rxring_size {
		/** qregs_rxring_size_rxring0_size: Size of RX ring zero, maximum 4095 */
		u16 rxring0_size;

		/** qregs_rxring_size_rxring1_size: Size of ring one, maximum 4095 */
		u16 rxring1_size;

	} rxring_size;

	/** qregs_rxring_low:  */
	struct qregs_rxring_low {
		/**
		 * qregs_rxring_low_rxring0_low: Trigger interrupt when number of free RX
		 * descs <= this, maximum 4095
		 */
		u16 rxring0_low;

		/**
		 * qregs_rxring_low_rxring1_low: Trigger interrupt when number of free RX
		 * descs <= this, maximum 4095
		 */
		u16 rxring1_low;

	} rxring_low;

	/** qregs_qchain1:  */
	struct qchain_regs qchain1;

	u8 unused_2[108];

	/** qregs_end_word:  */
	u32 end_word;

};

/**
 * Bitfield accessors for: qregs_qcfg bitfield_0
 */

enum qregs_qcfg_dma_pref {
	QREGS_QCFG_DMA_PREF_ROUND_ROBIN			= 0,
	QREGS_QCFG_DMA_PREF_FRX_TX1_TX0			= 1,
	QREGS_QCFG_DMA_PREF_TX1_FRX_TX0			= 2,
	QREGS_QCFG_DMA_PREF_TX1_TX0_FRX			= 3,
};
enum qregs_qcfg_burst_size {
	QREGS_QCFG_BURST_SIZE_16_BYTES			= 0,
	QREGS_QCFG_BURST_SIZE_32_BYTES			= 1,
	QREGS_QCFG_BURST_SIZE_64_BYTES			= 2,
	QREGS_QCFG_BURST_SIZE_128_BYTES			= 3,
};

#define QREGS_QCFG_RX_2B_OFFSET				BIT(31)
#define QREGS_QCFG_DMA_PREF_MASK			GENMASK(30, 29)
#define QREGS_QCFG_MSG_WORD_SWAP			BIT(28)
#define QREGS_QCFG_DSCP_BYTE_SWAP			BIT(27)
#define QREGS_QCFG_PAYLOAD_BYTE_SW			BIT(26)
#define QREGS_QCFG_VCHNL_MAP_EN				BIT(25)
#define QREGS_QCFG_VCHNL_MAP_MODE			BIT(24)
#define QREGS_QCFG_QDMA_LPBK_RXQ_SEL			BIT(22)
#define QREGS_QCFG_SLM_RELEASE_EN			BIT(21)
#define QREGS_QCFG_TX_IMMEDIATE_DONE			BIT(20)
#define QREGS_QCFG_IRQ_EN				BIT(19)
#define QREGS_QCFG_GDM_LOOPBACK				BIT(17)
#define QREGS_QCFG_QDMA_LOOPBACK			BIT(16)
#define QREGS_QCFG_CHECK_DONE				BIT(7)
#define QREGS_QCFG_TX_WB_DONE				BIT(6)
#define QREGS_QCFG_BURST_SIZE_MASK			GENMASK(5, 4)
#define QREGS_QCFG_RX_DMA_BUSY				BIT(3)
#define QREGS_QCFG_RX_DMA_EN				BIT(2)
#define QREGS_QCFG_TX_DMA_BUSY				BIT(1)
#define QREGS_QCFG_TX_DMA_EN				BIT(0)


/** If enabled, use (dscp_pkt_ptr + 2) as starting address for rx payload */
static inline bool is_qregs_qcfg_rx_2b_offset(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_RX_2B_OFFSET, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_2b_offset(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_2B_OFFSET, v);
}

/** DMA channel scheduling preference, FRX means "Forwarding and RX" */
static inline enum qregs_qcfg_dma_pref get_qregs_qcfg_dma_pref(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_DMA_PREF_MASK, x->bitfield_0);
}
static inline void set_qregs_qcfg_dma_pref(struct qregs_qcfg *x, enum qregs_qcfg_dma_pref v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_DMA_PREF_MASK, v);
}

/**
 * Enable message word swap, don't know what this does but every implementation
 * sets it on Big Endian.
 */
static inline bool is_qregs_qcfg_msg_word_swap(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_MSG_WORD_SWAP, x->bitfield_0);
}
static inline void set_qregs_qcfg_msg_word_swap(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_MSG_WORD_SWAP, v);
}

/**
 * Endian-swap packet descriptors (?), drivers always set this on Big Endian
 * machines.
 */
static inline bool is_qregs_qcfg_dscp_byte_swap(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_DSCP_BYTE_SWAP, x->bitfield_0);
}
static inline void set_qregs_qcfg_dscp_byte_swap(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_DSCP_BYTE_SWAP, v);
}

/**
 * Endian-swap payload bytes, drivers always set this on Big Endian machines.
 */
static inline bool is_qregs_qcfg_payload_byte_sw(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_PAYLOAD_BYTE_SW, x->bitfield_0);
}
static inline void set_qregs_qcfg_payload_byte_sw(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_PAYLOAD_BYTE_SW, v);
}

/** Enable virtual mapping to group queues per physical channel */
static inline bool is_qregs_qcfg_vchnl_map_en(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_VCHNL_MAP_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_vchnl_map_en(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_VCHNL_MAP_EN, v);
}

/** Map of 4 virtual channels per physical channel, 0 = map 2 */
static inline bool is_qregs_qcfg_vchnl_map_mode(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_VCHNL_MAP_MODE, x->bitfield_0);
}
static inline void set_qregs_qcfg_vchnl_map_mode(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_VCHNL_MAP_MODE, v);
}

/**
 * If enabled, qdma loopback goes to queue 1, otherwise it goes to queue zero
 */
static inline bool is_qregs_qcfg_qdma_lpbk_rxq_sel(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_QDMA_LPBK_RXQ_SEL, x->bitfield_0);
}
static inline void set_qregs_qcfg_qdma_lpbk_rxq_sel(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_QDMA_LPBK_RXQ_SEL, v);
}

/** Enable qdma fwd path release slm_block */
static inline bool is_qregs_qcfg_slm_release_en(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_SLM_RELEASE_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_slm_release_en(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_SLM_RELEASE_EN, v);
}

/** QDMA generate pkt_done itself instead of using pse pkt_done */
static inline bool is_qregs_qcfg_tx_immediate_done(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_TX_IMMEDIATE_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_immediate_done(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_IMMEDIATE_DONE, v);
}

/** Enable "interrupt queue" (i.e. Done List) for tx dma done */
static inline bool is_qregs_qcfg_irq_en(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_IRQ_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_irq_en(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_IRQ_EN, v);
}

/** Enable gdm loopback tx packet to rx path */
static inline bool is_qregs_qcfg_gdm_loopback(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_GDM_LOOPBACK, x->bitfield_0);
}
static inline void set_qregs_qcfg_gdm_loopback(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_GDM_LOOPBACK, v);
}

/** Enable hw qdma loopback tx packet to rx path */
static inline bool is_qregs_qcfg_qdma_loopback(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_QDMA_LOOPBACK, x->bitfield_0);
}
static inline void set_qregs_qcfg_qdma_loopback(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_QDMA_LOOPBACK, v);
}

/**
 * Check the done bit of descriptor and don't use descriptors which are marked
 * done. If disabled, the QDMA engine will determine if a descriptor is usable
 * based only on the ring pointers.
 */
static inline bool is_qregs_qcfg_check_done(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_CHECK_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_check_done(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_CHECK_DONE, v);
}

/**
 * Set the "done" bit in tx descriptor after sending. If disabled then the
 * engine will skip setting the done bit and rely on the driver to check the
 * Done List (i.e. `irq_en`).
 */
static inline bool is_qregs_qcfg_tx_wb_done(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_TX_WB_DONE, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_wb_done(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_WB_DONE, v);
}

/** Number of bytes per DMA burst */
static inline enum qregs_qcfg_burst_size get_qregs_qcfg_burst_size(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_BURST_SIZE_MASK, x->bitfield_0);
}
static inline void set_qregs_qcfg_burst_size(struct qregs_qcfg *x, enum qregs_qcfg_burst_size v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_BURST_SIZE_MASK, v);
}

/** RX DMA engine currently busy */
static inline bool is_qregs_qcfg_rx_dma_busy(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_RX_DMA_BUSY, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_dma_busy(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_DMA_BUSY, v);
}

/** Enable RX DMA */
static inline bool is_qregs_qcfg_rx_dma_en(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_RX_DMA_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_rx_dma_en(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_RX_DMA_EN, v);
}

/** TX DMA engine currently busy */
static inline bool is_qregs_qcfg_tx_dma_busy(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_TX_DMA_BUSY, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_dma_busy(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_DMA_BUSY, v);
}

/** Enable TX DMA */
static inline bool is_qregs_qcfg_tx_dma_en(struct qregs_qcfg *x) {
	return FIELD_GET(QREGS_QCFG_TX_DMA_EN, x->bitfield_0);
}
static inline void set_qregs_qcfg_tx_dma_en(struct qregs_qcfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_QCFG_TX_DMA_EN, v);
}

/**
 * Bitfield accessors for: struct hwf_cfg
 * Hardware forwarding configuration
 */

enum qregs_hwf_cfg_pkt_sz {
	QREGS_HWF_CFG_PKT_SZ_2048			= 0,
	QREGS_HWF_CFG_PKT_SZ_4096			= 1,
	QREGS_HWF_CFG_PKT_SZ_8192			= 2,
	QREGS_HWF_CFG_PKT_SZ_16384			= 3,
};

#define QREGS_HWF_CFG_PKT_SZ_MASK			GENMASK(29, 28)
#define QREGS_HWF_CFG_LOW_TH_MASK			GENMASK(12, 0)


/**
 * The size of the packet buffers in hwf_data_addr (and therefore the maximum
 * effective MTU).
 */
static inline enum qregs_hwf_cfg_pkt_sz get_qregs_hwf_cfg_pkt_sz(struct hwf_cfg *x) {
	return FIELD_GET(QREGS_HWF_CFG_PKT_SZ_MASK, x->word);
}
static inline void set_qregs_hwf_cfg_pkt_sz(struct hwf_cfg *x, enum qregs_hwf_cfg_pkt_sz v) {
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG_PKT_SZ_MASK, v);
}

/**
 * When number of available (not busy) hardware descriptors is below this,
 * generate an interrupt and pause hardware forwarding.
 */
static inline u16 get_qregs_hwf_cfg_low_th(struct hwf_cfg *x) {
	return FIELD_GET(QREGS_HWF_CFG_LOW_TH_MASK, x->word);
}
static inline void set_qregs_hwf_cfg_low_th(struct hwf_cfg *x, u16 v) {
	x->word = FIELD_SET(x->word, QREGS_HWF_CFG_LOW_TH_MASK, v);
}

/**
 * Bitfield accessors for: qregs_hwf_cfg1 bitfield_0
 */

#define QREGS_HWF_CFG1_START				BIT(7)
#define QREGS_HWF_CFG1_OVERHEAD_EN			BIT(0)


/** Start up the hardware forwarding subsystem */
static inline bool is_qregs_hwf_cfg1_start(struct qregs_hwf_cfg1 *x) {
	return FIELD_GET(QREGS_HWF_CFG1_START, x->bitfield_0);
}
static inline void set_qregs_hwf_cfg1_start(struct qregs_hwf_cfg1 *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_HWF_CFG1_START, v);
}

/** When set, add overhead to packet size for accounting purposes */
static inline bool is_qregs_hwf_cfg1_overhead_en(struct qregs_hwf_cfg1 *x) {
	return FIELD_GET(QREGS_HWF_CFG1_OVERHEAD_EN, x->bitfield_0);
}
static inline void set_qregs_hwf_cfg1_overhead_en(struct qregs_hwf_cfg1 *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_HWF_CFG1_OVERHEAD_EN, v);
}

/**
 * Bitfield accessors for: qregs_tx_congest_cfg bitfield_0
 * Configuration for TX congestion dropping
 */

enum qregs_tx_congest_cfg_dyncong_margin {
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_0_PCT	= 0,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_25_PCT	= 1,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_50_PCT	= 2,
	QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_100_PCT	= 3,
};
enum qregs_tx_congest_cfg_dyncong_dei_scale {
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_HALF	= 0,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_QUARTER	= 1,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_EIGHTH	= 2,
	QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_SIXTEENTH = 3,
};

#define QREGS_TX_CONGEST_CFG_TAIL_DROP_EN		BIT(15)
#define QREGS_TX_CONGEST_CFG_DEI_DROP_EN		BIT(14)
#define QREGS_TX_CONGEST_CFG_DYNCONG_EN			BIT(13)
#define QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1		BIT(11)
#define QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1		BIT(10)
#define QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0		BIT(9)
#define QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0		BIT(8)
#define QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK	GENMASK(7, 6)
#define QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK	GENMASK(5, 4)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR		BIT(2)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX		BIT(1)
#define QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK		BIT(0)

static inline bool is_qregs_tx_congest_cfg_tail_drop_en(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_TAIL_DROP_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_tail_drop_en(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_TAIL_DROP_EN, v);
}

/** Support 802.1ad DEI packet dropping */
static inline bool is_qregs_tx_congest_cfg_dei_drop_en(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DEI_DROP_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dei_drop_en(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DEI_DROP_EN, v);
}

/** Enable dynamic congestion algorithm */
static inline bool is_qregs_tx_congest_cfg_dyncong_en(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_EN, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_en(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_EN, v);
}

/**
 * Block TX Ring1 when TX buffer usage exceeds max threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_max_thr_blk_tx1(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_max_thr_blk_tx1(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX1, v);
}

/**
 * Block TX Ring1 when TX buffer usage exceeds min threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_min_thr_blk_tx1(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_min_thr_blk_tx1(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX1, v);
}

/**
 * Block TX Ring0 when TX buffer usage exceeds max threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_max_thr_blk_tx0(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_max_thr_blk_tx0(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MAX_THR_BLK_TX0, v);
}

/**
 * Block TX Ring0 when TX buffer usage exceeds min threshold (see
 * tx_congest_thr)
 */
static inline bool is_qregs_tx_congest_cfg_min_thr_blk_tx0(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_min_thr_blk_tx0(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_MIN_THR_BLK_TX0, v);
}
static inline enum qregs_tx_congest_cfg_dyncong_margin get_qregs_tx_congest_cfg_dyncong_margin(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_margin(struct qregs_tx_congest_cfg *x, enum qregs_tx_congest_cfg_dyncong_margin v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_MARGIN_MASK, v);
}
static inline enum qregs_tx_congest_cfg_dyncong_dei_scale get_qregs_tx_congest_cfg_dyncong_dei_scale(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_dei_scale(struct qregs_tx_congest_cfg *x, enum qregs_tx_congest_cfg_dyncong_dei_scale v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_MASK, v);
}

/** Update dyanmic congestion after each update to WRR weights. */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_wrr(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_wrr(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_WRR, v);
}

/** Update dyanmic congestion after each TX or RX */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_txrx(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_txrx(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TXRX, v);
}

/** Update dyanmic congestion after each tick */
static inline bool is_qregs_tx_congest_cfg_dyncong_upd_tick(struct qregs_tx_congest_cfg *x) {
	return FIELD_GET(QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK, x->bitfield_0);
}
static inline void set_qregs_tx_congest_cfg_dyncong_upd_tick(struct qregs_tx_congest_cfg *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, QREGS_TX_CONGEST_CFG_DYNCONG_UPD_TICK, v);
}

#endif