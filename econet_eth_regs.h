// SPDX-License-Identifier: GPL-2.0-only
#ifndef ECONET_ETH_REGS_H
#define ECONET_ETH_REGS_H

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/types.h>

#ifndef FIELD_SET
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))
#endif

/**
 * qdma_cfg
 * 
 *      3                     2                   1                   0
 *      1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   0 |O|dpr|S|W|P|E|M|N|L|A|D|I|B|C|K|    unused_2   |H|T|bsi|Y|X|F|G|
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   4 
 * 
 * @bitfield_0 (32 bit): 
 *   @rx_2b_offset "O" (bit 31): If enabled, use (dscp_pkt_ptr + 2) as starting
 *                               address for rx payload
 *   @dma_preference "dpr" (bits 30..29): DMA channel scheduling preference, FRX
 *                                        means "Forwarding and RX"
 *   @msg_word_swap "S" (bit 28): Enable message word swap, don't know what this
 *                                does but every implementation sets it on Big
 *                                Endian.
 *   @dscp_byte_swap "W" (bit 27): Endian-swap packet descriptors (?), drivers
 *                                 always set this on Big Endian machines.
 *   @payload_byte_sw "P" (bit 26): Endian-swap payload bytes, drivers always
 *                                  set this on Big Endian machines.
 *   @vchnl_map_en "E" (bit 25): Enable virtual mapping to group queues per
 *                               physical channel
 *   @vchnl_map_mode "M" (bit 24): Map of 4 virtual channels per physical
 *                                 channel, 0 = map 2
 *   @unused_0 "N" (bit 23): Reserved
 *   @qdma_lpbk_rxq_sel "L" (bit 22): If enabled, qdma loopback goes to queue 1,
 *                                    otherwise it goes to queue zero
 *   @slm_release_en "A" (bit 21): Enable qdma fwd path release slm_block
 *   @tx_immediate_done "D" (bit 20): QDMA generate pkt_done itself instead of
 *                                    using pse pkt_done
 *   @irq_en "I" (bit 19): Enable "interrupt queue" (i.e. Done List) for tx dma
 *                         done
 *   @unused_1 "B" (bit 18): Reserved
 *   @gdm_loopback "C" (bit 17): Enable gdm loopback tx packet to rx path
 *   @qdma_loopback "K" (bit 16): Enable hw qdma loopback tx packet to rx path
 *   @unused_2 (bits 15..8): Reserved
 *   @check_done "H" (bit 7): Check the done bit of descriptor and don't use
 *                            descriptors which are marked done. If disabled,
 *                            the QDMA engine will determine if a descriptor is
 *                            usable based only on the ring pointers.
 *   @tx_wb_done "T" (bit 6): Set the "done" bit in tx descriptor after sending.
 *                            If disabled then the engine will skip setting the
 *                            done bit and rely on the driver to check the Done
 *                            List (i.e. `irq_en`).
 *   @burst_size "bsi" (bits 5..4): Number of bytes per DMA burst
 *   @rx_dma_busy "Y" (bit 3): RX DMA engine currently busy
 *   @rx_dma_en "X" (bit 2): Enable RX DMA
 *   @tx_dma_busy "F" (bit 1): TX DMA engine currently busy
 *   @tx_dma_en "G" (bit 0): Enable TX DMA
 */


/* qdma_cfg bitfield_0 */

enum qcfg_dma_pref {
	QCFG_DMA_PREF_ROUND_ROBIN			= 0,
	QCFG_DMA_PREF_FRX_TX1_TX0			= 1,
	QCFG_DMA_PREF_TX1_FRX_TX0			= 2,
	QCFG_DMA_PREF_TX1_TX0_FRX			= 3,
};
enum qcfg_burst_size {
	QCFG_BURST_SIZE_16_BYTES			= 0,
	QCFG_BURST_SIZE_32_BYTES			= 1,
	QCFG_BURST_SIZE_64_BYTES			= 2,
	QCFG_BURST_SIZE_128_BYTES			= 3,
};

#define QCFG_RX_2B_OFFSET				BIT(31)
#define QCFG_DMA_PREF_MASK				GENMASK(30, 29)
#define QCFG_MSG_WORD_SWAP				BIT(28)
#define QCFG_DSCP_BYTE_SWAP				BIT(27)
#define QCFG_PAYLOAD_BYTE_SW				BIT(26)
#define QCFG_VCHNL_MAP_EN				BIT(25)
#define QCFG_VCHNL_MAP_MODE				BIT(24)
#define QCFG_QDMA_LPBK_RXQ_SEL				BIT(22)
#define QCFG_SLM_RELEASE_EN				BIT(21)
#define QCFG_TX_IMMEDIATE_DONE				BIT(20)
#define QCFG_IRQ_EN					BIT(19)
#define QCFG_GDM_LOOPBACK				BIT(17)
#define QCFG_QDMA_LOOPBACK				BIT(16)
#define QCFG_CHECK_DONE					BIT(7)
#define QCFG_TX_WB_DONE					BIT(6)
#define QCFG_BURST_SIZE_MASK				GENMASK(5, 4)
#define QCFG_RX_DMA_BUSY				BIT(3)
#define QCFG_RX_DMA_EN					BIT(2)
#define QCFG_TX_DMA_BUSY				BIT(1)
#define QCFG_TX_DMA_EN					BIT(0)



/**
 * qring - QDMA Ring Registers
 * 
 * @txbase (32 bit): TX descriptor array address
 * @rxbase (32 bit): RX descriptor array address
 * @tx_cpui (32 bit): TX ring CPU (driver) index
 * @tx_hwi (32 bit): TX ring hardware index
 * @rx_cpui (32 bit): RX ring CPU (driver) index
 * @rx_hwi (32 bit): TX ring hardware index
 */
struct qchain_regs {
        dma_addr_t txbase;
        dma_addr_t rxbase;
        u32 tx_cpui;
        u32 tx_hwi;
        u32 rx_cpui;
        u32 rx_hwi;
};

/**
 * qregs - QDMA Global Registers
 */
struct qregs {
	u32 version;
	u32 cfg;
	struct qchain_regs qchain0;
	u8 unused_0[232];
	struct qchain_regs qchain1;
	u8 unused_1[108];
	u32 end_word;
};

#endif /* ECONET_ETH_REGS_H */


_Static_assert(sizeof(struct qregs) == 0x190, "qdma_regs size mismatch");