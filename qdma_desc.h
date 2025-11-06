// SPDX-License-Identifier: GPL-2.0-only
#ifndef QDMA_DESC_H
#define QDMA_DESC_H

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/types.h>

#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))

/**
 * qdma_desc_etx
 * 
 *     3                     2                   1                   0
 *     1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  0 | nknwn |             sp_tag            |O|    channel    |queue|
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  4 |I|C|T|S|  udf_pmap |fport|E|vty|            vlan_tag           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  8 
 * 
 * @bitfield_0 (32 bit): 
 *   @unknown0 "nknwn" (bits 31..28): Unused, called "rev" probably short for
 *                                    reserved
 *   @sp_tag (bits 27..12): MediaTek "Special Tag" format which encapsulates
 *                          both switch port number and possible VLAN
 *   @oam "O" (bit 11): OAM (management) frame, never used with Ethernet
 *                      transmissions
 *   @channel (bits 10..3): The channel number for QoS prioritization
 *   @queue (bits 2..0): The queue number for QoS prioritization
 * @bitfield_1 (16 bit): 
 *   @ico "I" (1 bit): Checksum offload, probably IP
 *   @uco "C" (1 bit): Checksum offload, probably UDP
 *   @tco "T" (1 bit): Checksum offload, probably TCP
 *   @sco "S" (1 bit): Unknown, maybe SCTP checksum offload
 *   @udf_pmap (6 bit): Unknown / unused
 *   @fport (3 bit): Where in the Frame Engine to send the packet to Loopback is
 *                   immediate loopback, QDMA_LOOPBACK is loopback after QoS
 *                   reprioritization and QDMA_HW_LOOPBACK is loopback after (I
 *                   think) hardware forwarding. The most useful values are LAN,
 *                   WAN, and PPE (Packet Processing Engine).
 *   @vlan_en "E" (1 bit): If 1 then add a vlan header to the packet
 *   @vlan_type "vty" (2 bit): Which type of vlan to add to the packet header
 * @vlan_tag (16 bit): The VLAN number, if vlan_en is set
 */
struct qdma_desc_etx {
	u32 bitfield_0;
	u16 bitfield_1;
	u16 vlan_tag;
};

/* qdma_desc_etx bitfield_0 */

#define ETX_UNKNOWN0_MASK				GENMASK(31, 28)
#define ETX_SP_TAG_MASK					GENMASK(27, 12)
#define ETX_OAM						BIT(11)
#define ETX_CHANNEL_MASK				GENMASK(10, 3)
#define ETX_QUEUE_MASK					GENMASK(2, 0)

static inline u8 get_etx_unknown0(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_UNKNOWN0_MASK, x->bitfield_0);
}
static inline void set_etx_unknown0(struct qdma_desc_etx *x, u8 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_UNKNOWN0_MASK, v);
}
static inline u16 get_etx_sp_tag(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_SP_TAG_MASK, x->bitfield_0);
}
static inline void set_etx_sp_tag(struct qdma_desc_etx *x, u16 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_SP_TAG_MASK, v);
}
static inline bool is_etx_oam(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_OAM, x->bitfield_0);
}
static inline void set_etx_oam(struct qdma_desc_etx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_OAM, v);
}
static inline u8 get_etx_channel(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_CHANNEL_MASK, x->bitfield_0);
}
static inline void set_etx_channel(struct qdma_desc_etx *x, u8 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_CHANNEL_MASK, v);
}
static inline u8 get_etx_queue(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_QUEUE_MASK, x->bitfield_0);
}
static inline void set_etx_queue(struct qdma_desc_etx *x, u8 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ETX_QUEUE_MASK, v);
}

/* qdma_desc_etx bitfield_1 */

enum etx_fport {
	ETX_FPORT_LOOPBACK				= 0,
	ETX_FPORT_LAN					= 1,
	ETX_FPORT_WAN					= 2,
	ETX_FPORT_PPE					= 4,
	ETX_FPORT_QDMA_LOOPBACK				= 5,
	ETX_FPORT_QDMA_HW_LOOPBACK			= 6,
	ETX_FPORT_DROP					= 7,
};
enum etx_vlan_type {
	ETX_VLAN_TYPE_8100				= 0,
	ETX_VLAN_TYPE_9100				= 2,
	ETX_VLAN_TYPE_88A8				= 1,
	ETX_VLAN_TYPE_UNKNOWN				= 3,
};

#define ETX_ICO						BIT(15)
#define ETX_UCO						BIT(14)
#define ETX_TCO						BIT(13)
#define ETX_SCO						BIT(12)
#define ETX_UDF_PMAP_MASK				GENMASK(11, 6)
#define ETX_FPORT_MASK					GENMASK(5, 3)
#define ETX_VLAN_EN					BIT(2)
#define ETX_VLAN_TYPE_MASK				GENMASK(1, 0)

static inline bool is_etx_ico(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_ICO, x->bitfield_1);
}
static inline void set_etx_ico(struct qdma_desc_etx *x, bool v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_ICO, v);
}
static inline bool is_etx_uco(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_UCO, x->bitfield_1);
}
static inline void set_etx_uco(struct qdma_desc_etx *x, bool v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_UCO, v);
}
static inline bool is_etx_tco(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_TCO, x->bitfield_1);
}
static inline void set_etx_tco(struct qdma_desc_etx *x, bool v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_TCO, v);
}
static inline bool is_etx_sco(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_SCO, x->bitfield_1);
}
static inline void set_etx_sco(struct qdma_desc_etx *x, bool v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_SCO, v);
}
static inline u8 get_etx_udf_pmap(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_UDF_PMAP_MASK, x->bitfield_1);
}
static inline void set_etx_udf_pmap(struct qdma_desc_etx *x, u8 v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_UDF_PMAP_MASK, v);
}
static inline enum etx_fport get_etx_fport(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_FPORT_MASK, x->bitfield_1);
}
static inline void set_etx_fport(struct qdma_desc_etx *x, enum etx_fport v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_FPORT_MASK, v);
}
static inline bool is_etx_vlan_en(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_VLAN_EN, x->bitfield_1);
}
static inline void set_etx_vlan_en(struct qdma_desc_etx *x, bool v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_VLAN_EN, v);
}
static inline enum etx_vlan_type get_etx_vlan_type(struct qdma_desc_etx *x) {
	return FIELD_GET(ETX_VLAN_TYPE_MASK, x->bitfield_1);
}
static inline void set_etx_vlan_type(struct qdma_desc_etx *x, enum etx_vlan_type v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ETX_VLAN_TYPE_MASK, v);
}

/**
 * qdma_desc_erx
 * 
 *     3                     2                   1                   0
 *     1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  0 |                            unknown0                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  4 |nknwn|I|P|F|T|L|A| sport |   crsn  |         ppe_entry         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  8 |                           unknown2                          |N|
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 12 |             sp_tag            |              tci              |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 16 
 * 
 * @unknown0 (32 bit): Unknown / unused field, first word in descriptor
 * @bitfield_0 (32 bit): 
 *   @unknown1 "nknwn" (bits 31..29): Revision number? (unused)
 *   @ip6 "I" (bit 28): IPv6 packet indicator
 *   @ip4 "P" (bit 27): IPv4 packet indicator
 *   @ip4f "F" (bit 26): IPv4 fragment flag
 *   @tack "T" (bit 25): TCP ACK flag
 *   @l2vld "L" (bit 24): Layer 2 valid flag
 *   @l4f "A" (bit 23): Layer 4 flag (e.g., checksum failure)
 *   @sport (bits 22..19): Where the packet came from, mostly unknown / unused,
 *                         needs testing
 *   @crsn (bits 18..14): Most likely a MediaTek PPE CPU_REASON
 *   @ppe_entry (bits 13..0): PPE (Packet Processing Engine) entry index
 * @bitfield_1 (32 bit): 
 *   @unknown2 (bits 31..1): Reserved
 *   @untag "N" (bit 0): VLAN untag flag
 * @sp_tag (16 bit): MediaTek "Special Tag" for switch port/VLAN encoding
 * @tci (16 bit): The TCI of any vlan tag that was unpopped beneath the MTK
 *                "Special Tag"
 */
struct qdma_desc_erx {
	u32 unknown0;
	u32 bitfield_0;
	u32 bitfield_1;
	u16 sp_tag;
	u16 tci;
};

/* qdma_desc_erx bitfield_0 */

#define ERX_UNKNOWN1_MASK				GENMASK(31, 29)
#define ERX_IP6						BIT(28)
#define ERX_IP4						BIT(27)
#define ERX_IP4F					BIT(26)
#define ERX_TACK					BIT(25)
#define ERX_L2VLD					BIT(24)
#define ERX_L4F						BIT(23)
#define ERX_SPORT_MASK					GENMASK(22, 19)
#define ERX_CRSN_MASK					GENMASK(18, 14)
#define ERX_PPE_ENTRY_MASK				GENMASK(13, 0)

static inline u8 get_erx_unknown1(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_UNKNOWN1_MASK, x->bitfield_0);
}
static inline void set_erx_unknown1(struct qdma_desc_erx *x, u8 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_UNKNOWN1_MASK, v);
}
static inline bool is_erx_ip6(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_IP6, x->bitfield_0);
}
static inline void set_erx_ip6(struct qdma_desc_erx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_IP6, v);
}
static inline bool is_erx_ip4(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_IP4, x->bitfield_0);
}
static inline void set_erx_ip4(struct qdma_desc_erx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_IP4, v);
}
static inline bool is_erx_ip4f(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_IP4F, x->bitfield_0);
}
static inline void set_erx_ip4f(struct qdma_desc_erx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_IP4F, v);
}
static inline bool is_erx_tack(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_TACK, x->bitfield_0);
}
static inline void set_erx_tack(struct qdma_desc_erx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_TACK, v);
}
static inline bool is_erx_l2vld(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_L2VLD, x->bitfield_0);
}
static inline void set_erx_l2vld(struct qdma_desc_erx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_L2VLD, v);
}
static inline bool is_erx_l4f(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_L4F, x->bitfield_0);
}
static inline void set_erx_l4f(struct qdma_desc_erx *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_L4F, v);
}
static inline u8 get_erx_sport(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_SPORT_MASK, x->bitfield_0);
}
static inline void set_erx_sport(struct qdma_desc_erx *x, u8 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_SPORT_MASK, v);
}
static inline u8 get_erx_crsn(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_CRSN_MASK, x->bitfield_0);
}
static inline void set_erx_crsn(struct qdma_desc_erx *x, u8 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_CRSN_MASK, v);
}
static inline u16 get_erx_ppe_entry(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_PPE_ENTRY_MASK, x->bitfield_0);
}
static inline void set_erx_ppe_entry(struct qdma_desc_erx *x, u16 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, ERX_PPE_ENTRY_MASK, v);
}

/* qdma_desc_erx bitfield_1 */

#define ERX_UNKNOWN2_MASK				GENMASK(31, 1)
#define ERX_UNTAG					BIT(0)

static inline u32 get_erx_unknown2(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_UNKNOWN2_MASK, x->bitfield_1);
}
static inline void set_erx_unknown2(struct qdma_desc_erx *x, u32 v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ERX_UNKNOWN2_MASK, v);
}
static inline bool is_erx_untag(struct qdma_desc_erx *x) {
	return FIELD_GET(ERX_UNTAG, x->bitfield_1);
}
static inline void set_erx_untag(struct qdma_desc_erx *x, bool v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, ERX_UNTAG, v);
}

/**
 * qdma_desc - QDMA Packet Descriptor, Used to communicate an RX or TX message to the hardware.
 * 
 *     3                     2                   1                   0
 *     1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  0 |                            unknown0                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  4 |D|O|N|         unknown1        |            pkt_len            |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  8 |                            pkt_addr                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 12 |                unknown2               |        next_idx       |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 16 |                                                               |
 *    +                                                               +
 * 20 |                                                               |
 *    +                               t                               +
 * 24 |                                                               |
 *    +                                                               +
 * 28 |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 32 
 * 
 * @unknown0 (32 bit): Reserved / unused, we still export the symbol so we can
 *                     show it in debugging
 * @bitfield_0 (16 bit): 
 *   @done "D" (1 bit): Descriptor Done flag, this roughly means that the DSCP
 *                      "belongs to the driver", the hardware will set it when
 *                      it is done receiving or sending and will check to make
 *                      sure it's not touching a DSCP that is not meant for it.
 *                      This is not strictly necessary, you can determine which
 *                      packets are yours only through ring indexes and the TX
 *                      Done List, and setting and checking of this flag can be
 *                      deactivated.
 *   @dropped "O" (1 bit): Packet has been dropped
 *   @nls "N" (1 bit): Unknown meaning but used on EN761627 and EN7580
 *   @unknown1 (13 bit): Reserved / unused, we still export the symbol so we can
 *                       show it in debugging
 * @pkt_len (16 bit): Length of the packet in bytes
 * @pkt_addr (32 bit): Physical (DMA) address of the packet
 * @bitfield_1 (32 bit): 
 *   @unknown2 (bits 31..12): Reserved / unused, we still export the symbol so
 *                            we can show it in debugging
 *   @next_idx (bits 11..0): Index of the next descriptor in the ring.
 * @t (128 bit): This is either Ethernet RX, Ethernet TX, xPON RX, or xPON TX
 */
struct qdma_desc {
	u32 unknown0;
	u16 bitfield_0;
	u16 pkt_len;
	u32 pkt_addr;
	u32 bitfield_1;
	union {
		struct qdma_desc_erx erx;
		struct qdma_desc_etx etx;
		u32 raw[4];
	} t;
};

/* qdma_desc bitfield_0 */

#define DESC_DONE					BIT(15)
#define DESC_DROPPED					BIT(14)
#define DESC_NLS					BIT(13)
#define DESC_UNKNOWN1_MASK				GENMASK(12, 0)

static inline bool is_desc_done(struct qdma_desc *x) {
	return FIELD_GET(DESC_DONE, x->bitfield_0);
}
static inline void set_desc_done(struct qdma_desc *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, DESC_DONE, v);
}
static inline bool is_desc_dropped(struct qdma_desc *x) {
	return FIELD_GET(DESC_DROPPED, x->bitfield_0);
}
static inline void set_desc_dropped(struct qdma_desc *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, DESC_DROPPED, v);
}
static inline bool is_desc_nls(struct qdma_desc *x) {
	return FIELD_GET(DESC_NLS, x->bitfield_0);
}
static inline void set_desc_nls(struct qdma_desc *x, bool v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, DESC_NLS, v);
}
static inline u16 get_desc_unknown1(struct qdma_desc *x) {
	return FIELD_GET(DESC_UNKNOWN1_MASK, x->bitfield_0);
}
static inline void set_desc_unknown1(struct qdma_desc *x, u16 v) {
	x->bitfield_0 = FIELD_SET(x->bitfield_0, DESC_UNKNOWN1_MASK, v);
}

/* qdma_desc bitfield_1 */

#define DESC_UNKNOWN2_MASK				GENMASK(31, 12)
#define DESC_NEXT_IDX_MASK				GENMASK(11, 0)

static inline u32 get_desc_unknown2(struct qdma_desc *x) {
	return FIELD_GET(DESC_UNKNOWN2_MASK, x->bitfield_1);
}
static inline void set_desc_unknown2(struct qdma_desc *x, u32 v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, DESC_UNKNOWN2_MASK, v);
}
static inline u16 get_desc_next_idx(struct qdma_desc *x) {
	return FIELD_GET(DESC_NEXT_IDX_MASK, x->bitfield_1);
}
static inline void set_desc_next_idx(struct qdma_desc *x, u16 v) {
	x->bitfield_1 = FIELD_SET(x->bitfield_1, DESC_NEXT_IDX_MASK, v);
}

#endif /* QDMA_DESC_H */