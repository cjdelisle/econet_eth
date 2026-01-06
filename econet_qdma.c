// SPDX-License-Identifier: GPL-2.0-only
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <net/page_pool/helpers.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>

#include "linux/bitmap.h"
#include "linux/dev_printk.h"
#include "qdma_desc.h"
#include "qdma_regs.h"
#include "econet_eth.h"


#define AIROHA_MAX_NUM_IRQ_BANKS	1
#define QDMA_INT_REG_MAX		1

#define QDMA_FWD_DESC_SZ		16

// TODO: We don't know what this is yet
#define AIROHA_NUM_QOS_CHANNELS		4

struct airoha_queue_entry_rx {
	void *buf;
	dma_addr_t dma_addr;
	u16 dma_len;
};

struct airoha_queue_rx {
	struct en75_qdma *qdma;

	struct qchain_regs __iomem *qchain_regs;

	/* No lock, access only in NAPI, or else when NAPI is disabled
	 * and qdma->lock is held */
	struct airoha_queue_entry_rx *entry;
	struct desc *desc;
	u16 head;
	u16 tail;

	int queued;
	int ndesc;
	// int free_thr;
	int buf_size;

	struct napi_struct napi;
	struct page_pool *page_pool;
	struct sk_buff *skb;
};

struct airoha_queue_entry_tx {
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	u16 dma_len;
	u16 freelist_next;
};

struct airoha_queue_tx {
	struct en75_qdma *qdma;

	struct qchain_regs __iomem *qchain_regs;

	/* protect concurrent queue accesses
	 * use _bh unless in napi poll */
	spinlock_t lock_bh;
	struct airoha_queue_entry_tx *entry;
	struct desc *desc;

	/* The beginning of the list of free entries. */
	u16 freelist_head;
	u16 freelist_tail;
	// u16 head;
	// u16 tail;

	// int tqueued;
	int ndesc;
	// int free_thr;
	// int buf_size;

	struct napi_struct napi;
	// struct page_pool *page_pool;
	// struct sk_buff *skb;
};

struct airoha_irq_bank {
	struct en75_qdma *qdma;

	/* protect concurrent irqmask accesses
	 * use _irqsave unless in irq handler. */
	spinlock_t lock_irq;
	u32 irqmask[QDMA_INT_REG_MAX];
	u32 __iomem *mask_reg[QDMA_INT_REG_MAX];
	u32 __iomem *status_reg[QDMA_INT_REG_MAX];
	int irq;
};

struct airoha_tx_doneq {
	struct en75_qdma *qdma;

	struct napi_struct napi;

	int size;
	u32 *q;
};

struct en75_qdma {
	// struct airoha_eth *eth;
	struct en75_eth *eth;
	struct device *dev;

	int id;

	/* Protects register accesses, used in interrupt handler. */
	struct mutex lock;
	struct qregs __iomem *regs;
	/* Users and destroying are under the register lock. */
	int users;
	bool destroying;

	void *hwf_desc;

	// struct airoha_gdm_port *ports[AIROHA_MAX_NUM_GDM_PORTS];

	// atomic_t users;

	struct airoha_irq_bank irq_banks[AIROHA_MAX_NUM_IRQ_BANKS];

	struct airoha_tx_doneq q_tx_done[AIROHA_NUM_TX_DONE];

	struct airoha_queue_tx q_tx[AIROHA_NUM_TX_RING];
	struct airoha_queue_rx q_rx[AIROHA_NUM_RX_RING];

	struct net_device *napi_dev;

	struct en75_qdma_cfg cfg;
};

static void en75_fill_rx_queue(struct airoha_queue_rx *q)
{
	while (q->queued < q->ndesc - 1) {
		struct airoha_queue_entry_rx *e = &q->entry[q->head];
		struct desc *pdesc = &q->desc[q->head];
		struct page *page;
		int offset;
		int i;

		page = page_pool_dev_alloc_frag(q->page_pool, &offset,
						q->buf_size);
		if (!page)
			break;

		q->head = (q->head + 1) % q->ndesc;
		q->queued++;

		e->buf = page_address(page) + offset;
		e->dma_addr = page_pool_get_dma_addr(page) + offset;
		e->dma_len = SKB_WITH_OVERHEAD(q->buf_size);

		WRITE_ONCE(pdesc->info.pkt_len, e->dma_len);
		WRITE_ONCE(pdesc->pkt_addr, e->dma_addr);
		WRITE_ONCE(pdesc->next_idx, q->head);
		for (i = 0; i < ARRAY_SIZE(pdesc->msg.raw); i++)
			WRITE_ONCE(pdesc->msg.raw[i], 0);

		en75_wreg((u32)q->head, &q->qchain_regs->rx_cpui);
	}
}

static int airoha_qdma_rx_process(struct airoha_queue_rx *q, int budget)
{
	enum dma_data_direction dir = page_pool_get_dma_dir(q->page_pool);
	struct en75_qdma *qdma = q->qdma;
	// struct airoha_eth *eth = qdma->eth;
	int qid = q - &qdma->q_rx[0];
	int done = 0;

	while (done < budget) {
		struct airoha_queue_entry_rx *e = &q->entry[q->tail];
		struct page *page = virt_to_head_page(e->buf);
		struct desc *pdesc = &q->desc[q->tail];
		struct desc desc;
		int data_len, len;
		u32 hash; //, reason, msg1 = le32_to_cpu(desc->msg1);
		u8 sport;
		
		// u32 desc_ctrl = le32_to_cpu(desc->ctrl);
		
		// int data_len, len, p;
		memcpy(&desc, pdesc, sizeof(desc));
		if (!is_desc_info_done(&desc.info))
			break;

		// if (!(desc_ctrl & QDMA_DESC_DONE_MASK))
		// 	break;

		q->tail = (q->tail + 1) % q->ndesc;
		q->queued--;

		dma_sync_single_for_cpu(qdma->dev, e->dma_addr,
					SKB_WITH_OVERHEAD(q->buf_size), dir);

		len = desc.info.pkt_len;//FIELD_GET(QDMA_DESC_LEN_MASK, desc_ctrl);
		data_len = q->skb ? q->buf_size
				  : SKB_WITH_OVERHEAD(q->buf_size);
		if (!len || data_len < len)
			goto free_frag;

		// port = en75_get_sport_dev(qdma, &desc);
		// if (!port)
		// 	goto free_frag;

		if (!q->skb) { /* first buffer */
			q->skb = napi_build_skb(e->buf, q->buf_size);
			if (!q->skb)
				goto free_frag;

			__skb_put(q->skb, len);
			skb_mark_for_recycle(q->skb);
			q->skb->ip_summed = CHECKSUM_UNNECESSARY;
			skb_record_rx_queue(q->skb, qid);
		} else { /* scattered frame */
			struct skb_shared_info *shinfo = skb_shinfo(q->skb);
			int nr_frags = shinfo->nr_frags;

			if (nr_frags >= ARRAY_SIZE(shinfo->frags))
				goto free_frag;

			skb_add_rx_frag(q->skb, nr_frags, page,
					e->buf - page_address(page), len,
					q->buf_size);
		}

		// if (FIELD_GET(QDMA_DESC_MORE_MASK, desc_ctrl))
		if (is_desc_info_nls(&desc.info))
			continue;

		hash = get_erx_ppe_entry(&desc.msg.erx);
		// hash = FIELD_GET(AIROHA_RXD4_FOE_ENTRY, msg1);
		// if (hash != AIROHA_RXD4_FOE_ENTRY)
		skb_set_hash(q->skb, jhash_1word(hash, 0),
				PKT_HASH_TYPE_L4);

		// TODO: We're not controlling the PPE yet.
		// get_erx_crsn(&desc.t.erx)
		//
		// reason = FIELD_GET(AIROHA_RXD4_PPE_CPU_REASON, msg1);
		// if (reason == PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
		// 	airoha_ppe_check_skb(&eth->ppe->dev, q->skb, hash,
		// 			     false);

		sport = get_erx_sport(&desc.msg.erx);
		if (en75_rx_before_recv(qdma->eth, q->skb, sport))
			goto free_frag;

		done++;
		napi_gro_receive(&q->napi, q->skb);
		q->skb = NULL;
		continue;
free_frag:
		if (q->skb) {
			dev_kfree_skb(q->skb);
			q->skb = NULL;
		} else {
			page_pool_put_full_page(q->page_pool, page, true);
		}
	}
	en75_fill_rx_queue(q);

	return done;
}

#define RX1_DONE_INT 	BIT(5)
#define RX0_DONE_INT 	BIT(0)

union irq_purpose {
	struct {
		enum irq_purpose_type {
			IPS_INVAL = 0,
			IPS_DONE,
			IPS_LOW_DSCP,
			IPS_NO_DSCP,

			IPS_OVERFLOW,
			IPS_ERR_COHERENT,
			IPS_GPON_INT,
			IPS_EPON_INT,
			IPS_XPON_INT,
		} type : 16;

		/* If source is RX, TX, or DONE then chain is the number of the queue */
		int chain : 8;

		enum irq_purpose_source {
			IPSC_RX = 1,
			IPSC_TX,
			IPSC_DONE,
			IPSC_FWD,
			IPSC_UNSPEC,
		} source : 8;
	};
	u32 word;
};
static_assert(sizeof(union irq_purpose) == 4, "irq_purpose size");

static char *irq_purpose_type_str(enum irq_purpose_type t)
{
	switch (t) {
	case IPS_DONE:
		return "DONE";
	case IPS_LOW_DSCP:
		return "LOW_DSCP";
	case IPS_NO_DSCP:
		return "NO_DSCP";
	case IPS_OVERFLOW:
		return "OVERFLOW";
	case IPS_ERR_COHERENT:
		return "ERR_COHERENT";
	case IPS_GPON_INT:
		return "GPON_INT";
	case IPS_EPON_INT:
		return "EPON_INT";
	case IPS_XPON_INT:
		return "XPON_INT";
	case IPS_INVAL:
	default:
		return "INVAL";
	}
}

static char *irq_purpose_source_str(enum irq_purpose_source s)
{
	switch (s) {
	case IPSC_RX:
		return "RX";
	case IPSC_TX:
		return "TX";
	case IPSC_DONE:
		return "DONE";
	case IPSC_FWD:
		return "FWD";
	case IPSC_UNSPEC:
		return "UNSPEC";
	default:
		return "INVAL";
	}
}

#define IRQ_PURPOSE(t, s, c) \
	((union irq_purpose){ .type = IPS_ ## t, .source = IPSC_ ## s, .chain = (c) })

union irq_bit {
	struct {
		int bank 	: 8;
		int reg_idx 	: 8;
		int bit_idx 	: 8;
		int _pad 	: 8;
	};
	u32 word;
};
static_assert(sizeof(union irq_bit) == 4, "irq_bit size");

static const union irq_purpose EN751221_IRQ_MAP[] = {
	[0]  = IRQ_PURPOSE(DONE,		TX,	0),
	[1]  = IRQ_PURPOSE(DONE,		RX,	0),
	[2]  = IRQ_PURPOSE(NO_DSCP,		TX,	0),
	[3]  = IRQ_PURPOSE(NO_DSCP,		RX,	0),
	[4]  = IRQ_PURPOSE(DONE,		TX,	1),
	[5]  = IRQ_PURPOSE(DONE,		RX,	1),
	[6]  = IRQ_PURPOSE(NO_DSCP,		TX,	1),
	[7]  = IRQ_PURPOSE(NO_DSCP,		RX,	1),
	[8]  = IRQ_PURPOSE(NO_DSCP,		FWD,	-1),
	[9]  = IRQ_PURPOSE(NO_DSCP,		DONE,	0),
	[10] = IRQ_PURPOSE(LOW_DSCP,		FWD,	0),
	[11] = IRQ_PURPOSE(OVERFLOW,		UNSPEC,	-1),
	[12] = IRQ_PURPOSE(ERR_COHERENT,	TX,	0),
	[13] = IRQ_PURPOSE(ERR_COHERENT,	RX,	0),
	[14] = IRQ_PURPOSE(ERR_COHERENT,	TX,	1),
	[15] = IRQ_PURPOSE(ERR_COHERENT,	RX,	1),
	[16] = IRQ_PURPOSE(GPON_INT,		UNSPEC,	-1),
	[17] = IRQ_PURPOSE(EPON_INT,		UNSPEC,	-1),
	[18] = IRQ_PURPOSE(XPON_INT,		UNSPEC,	-1),
};

static union irq_purpose en751221_irq_purpose(union irq_bit bit)
{
	if (WARN_ON_ONCE(bit.bank != 0 || bit.reg_idx != 0 ||
			 bit.bit_idx > ARRAY_SIZE(EN751221_IRQ_MAP)))
		return (union irq_purpose){0};

	return EN751221_IRQ_MAP[bit.bit_idx];
}

static union irq_bit en751221_irq_bit(union irq_purpose purpose)
{
	union irq_bit bit = {0};
	for (int i = 0; i < ARRAY_SIZE(EN751221_IRQ_MAP); i++) {
		if (EN751221_IRQ_MAP[i].word == purpose.word) {
			bit.bit_idx = i;
			return bit;
		}
	}
	return bit;
}

static void airoha_qdma_set_irqmask(struct en75_qdma *qdma, union irq_bit b, bool enable)
{
	struct airoha_irq_bank *irq_bank;

	if (WARN_ON_ONCE(b.bank >= ARRAY_SIZE(qdma->irq_banks)))
		return;

	irq_bank = &qdma->irq_banks[b.bank];

	if (WARN_ON_ONCE(b.reg_idx >= ARRAY_SIZE(irq_bank->irqmask)))
		return;

	if (WARN_ON_ONCE(b.bit_idx >= 32))
		return;

	guard(spinlock_irqsave)(&irq_bank->lock_irq);

	if (enable)
		irq_bank->irqmask[b.reg_idx] |= BIT(b.bit_idx);
	else
		irq_bank->irqmask[b.reg_idx] &= ~BIT(b.bit_idx);

	en75_wreg(irq_bank->irqmask[b.reg_idx], irq_bank->mask_reg[b.reg_idx]);

	/* Read irq_enable register in order to guarantee the update above
	 * completes in the spinlock critical section.
	 */
	en75_rreg(irq_bank->mask_reg[b.reg_idx]);
}

static int airoha_qdma_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct airoha_queue_rx *q = container_of(napi, struct airoha_queue_rx, napi);
	struct en75_qdma *qdma = q->qdma;
	int cur, done = 0;

	do {
		cur = airoha_qdma_rx_process(q, budget - done);
		done += cur;
	} while (cur && done < budget);

	if (done < budget && napi_complete(napi)) {
		union irq_purpose purpose = IRQ_PURPOSE(DONE, RX, q - &qdma->q_rx[0]);
		union irq_bit b = en751221_irq_bit(purpose);
		airoha_qdma_set_irqmask(qdma, b, true);
	}

	return done;
}

static irqreturn_t airoha_irq_handler(int irq, void *dev_instance)
{
	struct airoha_irq_bank *irq_bank = dev_instance;
	struct en75_qdma *qdma = irq_bank->qdma;
	// u32 rx_intr_mask = 0, rx_intr1, rx_intr2;
	u32 intr[ARRAY_SIZE(irq_bank->irqmask)];
	int i;

	// if (!test_bit(DEV_STATE_INITIALIZED, &qdma->eth->state))
	// 	return IRQ_NONE;

	guard(spinlock)(&irq_bank->lock_irq);

	for (i = 0; i < ARRAY_SIZE(intr); i++) {
		unsigned long regval;
		u32 disable_int = 0;
		u8 bit;

		regval = en75_rreg(&qdma->regs->int_status);
		regval &= irq_bank->irqmask[i];

		for_each_set_bit(bit, &regval, 32) {
			union irq_purpose p = en751221_irq_purpose((union irq_bit){
				.bank = irq_bank - &qdma->irq_banks[0],
				.reg_idx = i,
				.bit_idx = bit,
			});

			if (p.type == IPS_DONE && p.source == IPSC_RX) {
				napi_schedule_irqoff(&qdma->q_rx[p.chain].napi);
				disable_int |= BIT(bit);
			} else if (p.type == IPS_DONE && p.source == IPSC_TX) {
				napi_schedule_irqoff(&qdma->q_tx_done[i].napi);
				disable_int |= BIT(bit);
			} else {
				dev_warn(qdma->dev, "%s IRQ from %s[%d]\n",
					 irq_purpose_type_str(p.type),
					 irq_purpose_source_str(p.source),
					 p.chain);			
			}
		}

		en75_wreg(intr[i], irq_bank->status_reg[i]);
		en75_wreg(~disable_int, irq_bank->mask_reg[i]);
	}

	return IRQ_HANDLED;
}



#define IRQ_RING_IDX_MASK		GENMASK(20, 16)
#define IRQ_DESC_IDX_MASK		GENMASK(15, 0)

static int en75_poll_tx_complete(struct napi_struct *napi, int budget)
{
	struct qregs_doneq_state state;
	struct airoha_tx_doneq *done_q;
	struct en75_qdma *qdma;
	int id, irq_queued;
	u32 done = 0, head;

	done_q = container_of(napi, struct airoha_tx_doneq, napi);
	qdma = done_q->qdma;
	id = done_q - &qdma->q_tx_done[0];

	state = en75_rreg(&qdma->regs->done_queue.state);
	head = state.head_index;
	head = head % done_q->size;
	irq_queued = state.length;

	while (irq_queued > 0 && done < budget) {
		u32 index, qid, val = done_q->q[head];
		struct airoha_queue_entry_tx *e;
		struct airoha_queue_tx *q;
		struct netdev_queue *txq;
		struct sk_buff *skb;
		struct desc desc;

		if (val == 0xffffffffU)
			break;

		done_q->q[head] = 0xffffffffU; /* mark as done */
		head = (head + 1) % done_q->size;
		irq_queued--;
		done++;

		qid = FIELD_GET(IRQ_RING_IDX_MASK, val);
		if (WARN_ON_ONCE(qid >= ARRAY_SIZE(qdma->q_tx)))
			continue;

		q = &qdma->q_tx[qid];
		if (WARN_ON_ONCE(!q->ndesc))
			continue;

		index = FIELD_GET(IRQ_DESC_IDX_MASK, val);
		if (WARN_ON_ONCE(index >= q->ndesc))
			continue;

		guard(spinlock)(&q->lock_bh);

		desc = q->desc[index];

		if (WARN_ON_ONCE(!is_desc_info_done(&desc.info) &&
				 !is_desc_info_dropped(&desc.info)))
			continue;

		e = &q->entry[index];
		skb = e->skb;

		dma_unmap_single(qdma->dev, e->dma_addr, e->dma_len,
				 DMA_TO_DEVICE);
		memset(e, 0, sizeof(*e));

		/* Completion ring can report out of order when hw QoS is
		 * enabled and packets with different priority are queued
		 * to same DMA ring. So we use a linked list to maintain free
		 * entries.
		 */
		e->freelist_next = 0xffff;
		q->entry[q->freelist_tail].freelist_next = index;
		q->freelist_tail = index;

		// TODO: This should not be necessary.
		// WRITE_ONCE(desc->msg0, 0);
		// WRITE_ONCE(desc->msg1, 0);

		txq = netdev_get_tx_queue(skb->dev,
					  skb_get_queue_mapping(skb));
		netdev_tx_completed_queue(txq, 1, skb->len);
		if (netif_tx_queue_stopped(txq))
			netif_tx_wake_queue(txq);

		dev_kfree_skb_any(skb);
	}

	if (done) {
		int i, len = done >> 7;

		for (i = 0; i < len; i++) {
			en75_rreg(&qdma->regs->done_queue.pop_back);
			en75_wreg(0x80U, &qdma->regs->done_queue.pop_back);
		}
		en75_rreg(&qdma->regs->done_queue.pop_back);
		en75_wreg(done & 0x7f, &qdma->regs->done_queue.pop_back);
	}

	if (done < budget && napi_complete(napi)) {
		union irq_purpose purpose = IRQ_PURPOSE(DONE, TX, id);
		union irq_bit b = en751221_irq_bit(purpose);
		airoha_qdma_set_irqmask(qdma, b, true);
	}

	return done;
}

/* Init functions, no locks, assumed non-concurrent */

static int en75_init_rx_queue(struct airoha_queue_rx *q,
			      struct en75_qdma *qdma, int ndesc)
{
	const struct page_pool_params pp_params = {
		.order = 0,
		.pool_size = 256,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.dma_dir = DMA_FROM_DEVICE,
		.max_len = PAGE_SIZE,
		.nid = NUMA_NO_NODE,
		.dev = qdma->dev,
		.napi = &q->napi,
	};
	int threshold = clamp(ndesc >> 3, 1, 32);
	struct qregs_rxring_size rrs;
	struct qregs_rxring_low rrl;
	dma_addr_t dma_addr;

	// TODO: Should be based on MTU
	q->buf_size = PAGE_SIZE / 2;
	q->ndesc = ndesc;
	q->qdma = qdma;
	q->qchain_regs = (q == &qdma->q_rx[0]) ?
			 &qdma->regs->qchain0 :
			 &qdma->regs->qchain1;

	q->entry = devm_kzalloc(qdma->dev, q->ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(q->page_pool)) {
		int err = PTR_ERR(q->page_pool);

		q->page_pool = NULL;
		return err;
	}

	q->desc = dmam_alloc_coherent(qdma->dev, q->ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	netif_napi_add(qdma->napi_dev, &q->napi, airoha_qdma_rx_napi_poll);

	en75_wreg(dma_addr, &q->qchain_regs->rxbase);
	en75_wreg(0U, &q->qchain_regs->rx_cpui);
	en75_wreg(0U, &q->qchain_regs->rx_hwi);

	rrs = en75_rreg(&qdma->regs->rxring_size);
	rrl = en75_rreg(&qdma->regs->rxring_low);

	if (q == &qdma->q_rx[0]) {
		rrs.rxring0_size = ndesc;
		rrl.rxring0_low = threshold;
	} else {
		rrs.rxring1_size = ndesc;
		rrl.rxring1_low = threshold;
	}

	en75_wreg(rrs, &qdma->regs->rxring_size);
	en75_wreg(rrl, &qdma->regs->rxring_low);

	en75_fill_rx_queue(q);

	return 0;
}

static int en75_init_irq_banks(struct platform_device *pdev,
			       struct en75_qdma *qdma,
			       int *irqs, int num_irqs)
{
	// struct airoha_eth *eth = qdma->eth;
	int i;//, id = qdma - &eth->qdma[0];

	if (num_irqs < ARRAY_SIZE(qdma->irq_banks))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(qdma->irq_banks); i++) {
		struct airoha_irq_bank *irq_bank = &qdma->irq_banks[i];
		int err; //, irq_index = 4 * id + i;
		const char *name;

		spin_lock_init(&irq_bank->lock_irq);
		irq_bank->qdma = qdma;

		irq_bank->irq = irqs[i];
		if (irq_bank->irq < 0)
			return irq_bank->irq;

		name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
				      KBUILD_MODNAME "/%d.%d", qdma->id, i);
		if (!name)
			return -ENOMEM;

		err = devm_request_irq(&pdev->dev, irq_bank->irq,
				       airoha_irq_handler, IRQF_SHARED, name,
				       irq_bank);
		if (err)
			return err;
	}

	return 0;
}

static int en75_init_tx_doneq(struct airoha_tx_doneq *done_q,
			      struct en75_qdma *qdma, int size)
{
	// int id = done_q - &qdma->q_tx_done[0];
	// struct airoha_eth *eth = qdma->eth;
	dma_addr_t dma_addr;

	netif_napi_add_tx(qdma->napi_dev, &done_q->napi,
			  en75_poll_tx_complete);
	done_q->q = dmam_alloc_coherent(qdma->dev, size * sizeof(u32),
				       &dma_addr, GFP_KERNEL);
	if (!done_q->q)
		return -ENOMEM;

	memset(done_q->q, 0xff, size * sizeof(u32));
	done_q->size = size;
	done_q->qdma = qdma;

	en75_wreg(dma_addr, &qdma->regs->done_queue.address);
	struct qregs_doneq_cfg cfg = en75_rreg(&qdma->regs->done_queue.config);
	cfg.size = size;
	cfg.int_threshold = 1;
	en75_wreg(cfg, &qdma->regs->done_queue.config);

	// qdma->regs->done_queue.address
	// airoha_qdma_wr(qdma, REG_TX_IRQ_BASE(id), dma_addr);
	// airoha_qdma_rmw(qdma, REG_TX_IRQ_CFG(id), TX_IRQ_DEPTH_MASK,
	// 		FIELD_PREP(TX_IRQ_DEPTH_MASK, size));
	// airoha_qdma_rmw(qdma, REG_TX_IRQ_CFG(id), TX_IRQ_THR_MASK,
	// 		FIELD_PREP(TX_IRQ_THR_MASK, 1));

	return 0;
}

static int en75_init_tx_queue(struct airoha_queue_tx *q,
			      struct en75_qdma *qdma, int size)
{
	// struct airoha_eth *eth = qdma->eth;
	int i, qid = q - &qdma->q_tx[0];
	dma_addr_t dma_addr;

	spin_lock_init(&q->lock_bh);
	q->ndesc = size;
	q->qdma = qdma;
	q->qchain_regs = (qid == 0) ?
			 &qdma->regs->qchain0 :
			 &qdma->regs->qchain1;

	q->entry = devm_kzalloc(qdma->dev, q->ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->desc = dmam_alloc_coherent(qdma->dev, q->ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	// TODO: This is probably not necessary
	struct desc desc = {0};
	set_desc_info_done(&desc.info, true);
	for (i = 0; i < q->ndesc; i++) {
		memcpy(&q->desc[i], &desc, sizeof(desc));
		// val = FIELD_PREP(QDMA_DESC_DONE_MASK, 1);
		// WRITE_ONCE(q->desc[i].ctrl, cpu_to_le32(val));
	}

	for (i = 0; i < q->ndesc - 1; i++) {
		q->entry[i].freelist_next = i + 1;
	}
	q->entry[q->ndesc - 1].freelist_next = 0xffff;
	q->freelist_tail = q->ndesc - 1;
	q->freelist_head = 0;



	// EN7580 only
	// /* xmit ring drop default setting */
	// airoha_qdma_set(qdma, REG_TX_RING_BLOCKING(qid),
	// 		TX_RING_IRQ_BLOCKING_TX_DROP_EN_MASK);

	en75_wreg(dma_addr, &q->qchain_regs->txbase);
	en75_wreg(0U, &q->qchain_regs->tx_cpui);
	en75_wreg(0U, &q->qchain_regs->tx_hwi);

	// airoha_qdma_wr(qdma, REG_TX_RING_BASE(qid), dma_addr);
	// airoha_qdma_rmw(qdma, REG_TX_CPU_IDX(qid), TX_RING_CPU_IDX_MASK,
	// 		FIELD_PREP(TX_RING_CPU_IDX_MASK, q->head));
	// airoha_qdma_rmw(qdma, REG_TX_DMA_IDX(qid), TX_RING_DMA_IDX_MASK,
	// 		FIELD_PREP(TX_RING_DMA_IDX_MASK, q->head));

	return 0;
}

static int en75_init_hw_fwd(struct en75_qdma *qdma)
{
	int size, index, num_desc = qdma->cfg.num_fwd_descs; // HW_DSCP_NUM;
	// struct airoha_eth *eth = qdma->eth;
	// int id = qdma - &eth->qdma[0];
	enum qregs_hwf_cfg_pkt_sz buf_size_cfg = 0;
	struct qregs_hwf_cfg1 cfg1;
	dma_addr_t dma_addr;
	u32 buf_size = 2048;
	struct hwf_cfg cfg;
	const char *name;

	name = devm_kasprintf(qdma->dev, GFP_KERNEL, "qdma%d-buf", qdma->id);
	if (!name)
		return -ENOMEM;

	while (buf_size < qdma->cfg.fwd_max_packet_size) {
		buf_size_cfg++;
		buf_size = 2048 << buf_size_cfg;
		if (buf_size > 16384) {
			dev_err(qdma->dev, "Unsupported hw forwarding max packet size %d\n",
				qdma->cfg.fwd_max_packet_size);
			return -EINVAL;
		}
	}

	index = of_property_match_string(qdma->dev->of_node,
					 "memory-region-names", name);
	if (index >= 0) {
		struct reserved_mem *rmem;
		struct device_node *np;

		/* Consume reserved memory for hw forwarding buffers queue if
		 * available in the DTS
		 */
		np = of_parse_phandle(qdma->dev->of_node, "memory-region",
				      index);
		if (!np)
			return -ENODEV;

		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
		dma_addr = rmem->base;
		/* Compute the number of hw descriptors according to the
		 * reserved memory size and the payload buffer size
		 */
		num_desc = div_u64(rmem->size, buf_size);

		if (num_desc < qdma->cfg.num_fwd_descs)
			dev_warn(qdma->dev, 
				 "Reserved memory %pa too small for %d "
				 "hw forwarding descriptors with %d bytes"
				 "payload, reducing to %d descriptors.\n",
				 &rmem->size, qdma->cfg.num_fwd_descs,
				 buf_size, num_desc);
	} else {
		size = buf_size * num_desc;
		if (!dmam_alloc_coherent(qdma->dev, size, &dma_addr,
					 GFP_KERNEL))
			return -ENOMEM;
	}

	en75_wreg(dma_addr, &qdma->regs->hwf_data_addr);
	// airoha_qdma_wr(qdma, REG_FWD_BUF_BASE, dma_addr);

	size = num_desc * QDMA_FWD_DESC_SZ;
	qdma->hwf_desc = dmam_alloc_coherent(qdma->dev, size, &dma_addr, GFP_KERNEL);
	if (!qdma->hwf_desc)
		return -ENOMEM;

	en75_wreg(dma_addr, &qdma->regs->hwf_desc_addr);
	// airoha_qdma_wr(qdma, REG_FWD_DSCP_BASE, dma_addr);


	cfg = en75_rreg(&qdma->regs->hwf_cfg);
	set_qregs_hwf_cfg_pkt_sz(&cfg, buf_size_cfg);
	set_qregs_hwf_cfg_low_th(&cfg, qdma->cfg.fwd_low_threshold);
	en75_wreg(cfg, &qdma->regs->hwf_cfg);

	cfg1 = en75_rreg(&qdma->regs->hwf_cfg1);
	cfg1.fwd_desc_n = num_desc;
	set_qregs_hwf_cfg1_start(&cfg1, true);

	// /* QDMA0: 2KB. QDMA1: 1KB */
	// airoha_qdma_rmw(qdma, REG_HW_FWD_DSCP_CFG,
	// 		HW_FWD_DSCP_PAYLOAD_SIZE_MASK,
	// 		FIELD_PREP(HW_FWD_DSCP_PAYLOAD_SIZE_MASK, !!id));
	// airoha_qdma_rmw(qdma, REG_FWD_DSCP_LOW_THR, FWD_DSCP_LOW_THR_MASK,
	// 		FIELD_PREP(FWD_DSCP_LOW_THR_MASK, 128));
	// airoha_qdma_rmw(qdma, REG_LMGR_INIT_CFG,
	// 		LMGR_INIT_START | LMGR_SRAM_MODE_MASK |
	// 		HW_FWD_DESC_NUM_MASK,
	// 		FIELD_PREP(HW_FWD_DESC_NUM_MASK, num_desc) |
	// 		LMGR_INIT_START | LMGR_SRAM_MODE_MASK);

	return read_poll_timeout(en75_rreg, cfg1,
				 !(cfg1.bitfield_0 & QREGS_HWF_CFG1_START), USEC_PER_MSEC,
				 30 * USEC_PER_MSEC, true, &qdma->regs->hwf_cfg1);

	// return read_poll_timeout(airoha_qdma_rr, status,
	// 			 !(status & LMGR_INIT_START), USEC_PER_MSEC,
	// 			 30 * USEC_PER_MSEC, true, qdma,
	// 			 REG_LMGR_INIT_CFG);
}

static int en75_init_final(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	for (i = 0; i < ARRAY_SIZE(qdma->irq_banks); i++) {
		int j;

		/* clear pending irqs */
		for (j = 0; j < ARRAY_SIZE(qdma->irq_banks[i].status_reg); j++)
			en75_wreg(0xffffffffU, qdma->irq_banks[i].status_reg[j]);

		/* enable IRQs */
		for (j = 0; j < ARRAY_SIZE(qdma->irq_banks[i].irqmask); j++) {
			u32 mask = 0;

			for (int k = 0; k < 32; k++) {
				union irq_bit bit = {
					.bank = i,
					.reg_idx = j,
					.bit_idx = k,
				};
				union irq_purpose p;
				u32 en = 0;
				
				p = en751221_irq_purpose(bit);

				/* RX and TX done */
				en |= (p.type == IPS_DONE);

				/* Running out of resources */
				en |= (p.type == IPS_NO_DSCP);
				en |= (p.type == IPS_LOW_DSCP);

				/* Error conditions */
				en |= (p.type == IPS_ERR_COHERENT);
				en |= (p.type == IPS_OVERFLOW);

				/* External hardware */
				en |= (p.type == IPS_GPON_INT);
				en |= (p.type == IPS_EPON_INT);
				en |= (p.type == IPS_XPON_INT);

				mask |= en << k;
			}

			qdma->irq_banks[i].irqmask[j] = mask;
			en75_wreg(mask, qdma->irq_banks[i].mask_reg[j]);
		}
		// airoha_qdma_wr(qdma, REG_INT_STATUS(i), 0xffffffff);
		/* setup rx irqs */
		// airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX0,
		// 		       INT_RX0_MASK(RX_IRQ_BANK_PIN_MASK(i)));
		// airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX1,
		// 		       INT_RX1_MASK(RX_IRQ_BANK_PIN_MASK(i)));
		// airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX2,
		// 		       INT_RX2_MASK(RX_IRQ_BANK_PIN_MASK(i)));
		// airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX3,
		// 		       INT_RX3_MASK(RX_IRQ_BANK_PIN_MASK(i)));
	}
	/* setup tx irqs */
	// airoha_qdma_irq_enable(&qdma->irq_banks[0], QDMA_INT_REG_IDX0,
	// 		       TX_COHERENT_LOW_INT_MASK | INT_TX_MASK);
	// airoha_qdma_irq_enable(&qdma->irq_banks[0], QDMA_INT_REG_IDX4,
	// 		       TX_COHERENT_HIGH_INT_MASK);

	// /* setup irq binding */
	// for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
	// 	if (!qdma->q_tx[i].ndesc)
	// 		continue;

	// 	if (TX_RING_IRQ_BLOCKING_MAP_MASK & BIT(i))
	// 		airoha_qdma_set(qdma, REG_TX_RING_BLOCKING(i),
	// 				TX_RING_IRQ_BLOCKING_CFG_MASK);
	// 	else
	// 		airoha_qdma_clear(qdma, REG_TX_RING_BLOCKING(i),
	// 				  TX_RING_IRQ_BLOCKING_CFG_MASK);
	// }

	// QCFG_RX_2B_OFFSET
	qcfg = (struct qregs_qcfg) { 0 };
	set_qregs_qcfg_msg_word_swap(&qcfg, true);
	set_qregs_qcfg_dscp_byte_swap(&qcfg, true);
	set_qregs_qcfg_payload_byte_sw(&qcfg, true);
	set_qregs_qcfg_irq_en(&qcfg, true);
	set_qregs_qcfg_check_done(&qcfg, true);
	set_qregs_qcfg_tx_wb_done(&qcfg, true);
	set_qregs_qcfg_burst_size(&qcfg, QREGS_QCFG_BURST_SIZE_128_BYTES);
	// set_qregs_qcfg_rx_dma_en(&qcfg, true); // set on qdma_use
	// set_qregs_qcfg_tx_dma_en(&qcfg, true);
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	// airoha_qdma_wr(qdma, REG_QDMA_GLOBAL_CFG,
	// 	       FIELD_PREP(GLOBAL_CFG_DMA_PREFERENCE_MASK, 3) |
	// 	       GLOBAL_CFG_CPU_TXR_RR_MASK |
	// 	       GLOBAL_CFG_PAYLOAD_BYTE_SWAP_MASK |
	// 	       GLOBAL_CFG_MULTICAST_MODIFY_FP_MASK |
	// 	       GLOBAL_CFG_MULTICAST_EN_MASK |
	// 	       GLOBAL_CFG_IRQ0_EN_MASK | GLOBAL_CFG_IRQ1_EN_MASK |
	// 	       GLOBAL_CFG_TX_WB_DONE_MASK |
	// 	       FIELD_PREP(GLOBAL_CFG_MAX_ISSUE_NUM_MASK, 2));

	// TODO
	//airoha_qdma_init_qos(qdma);

	en75_wreg(0U, &qdma->regs->rx_int_delay);
	/* disable qdma rx delay interrupt */
	// for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
	// 	if (!qdma->q_rx[i].ndesc)
	// 		continue;

	// 	airoha_qdma_clear(qdma, REG_RX_DELAY_INT_IDX(i),
	// 			  RX_DELAY_INT_MASK);
	// }

	struct qregs_tx_congest_cfg cngst_cfg = {0};
	set_qregs_tx_congest_cfg_tail_drop_en(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dei_drop_en(&cngst_cfg, true);
	en75_wreg(cngst_cfg, &qdma->regs->tx_congest_cfg);

	// airoha_qdma_set(qdma, REG_TXQ_CNGST_CFG,
	// 		TXQ_CNGST_DROP_EN | TXQ_CNGST_DEI_DROP_EN);

	// TODO
	// airoha_qdma_init_qos_stats(qdma);

	return 0;
}

static int en75_init(struct platform_device *pdev,
		     struct en75_qdma *qdma,
		     int *irqs,
		     int num_irqs)
{
	int err; //, id = qdma - &eth->qdma[0];
	int i;
	// const char *res;

	// // qdma->eth = eth;
	// res = devm_kasprintf(pdev->dev, GFP_KERNEL, "qdma%d", id);
	// if (!res)
	// 	return -ENOMEM;

	//devm_platform_ioremap_resource_byname(pdev, res);
	// if (IS_ERR(qdma->regs))
	// 	return dev_err_probe(eth->dev, PTR_ERR(qdma->regs),
	// 			     "failed to iomap qdma%d regs\n", id);

	err = en75_init_irq_banks(pdev, qdma, irqs, num_irqs);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		err = en75_init_rx_queue(&qdma->q_rx[i], qdma,
					 qdma->cfg.num_rx_descs[i]);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++) {
		err = en75_init_tx_doneq(&qdma->q_tx_done[i], qdma,
					 qdma->cfg.done_list_size[i]);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		err = en75_init_tx_queue(&qdma->q_tx[i], qdma,
					 qdma->cfg.num_tx_descs[i]);
		if (err)
			return err;
	}

	err = en75_init_hw_fwd(qdma);
	if (err)
		return err;

	return en75_init_final(qdma);
}

/* End init functions */

static int en75_qdma_destroy_locked(struct en75_qdma *qdma)
{
	int i;

	if (qdma->users) {
		qdma->destroying = true;
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		struct airoha_queue_rx *q = &qdma->q_rx[i];

		if (q->napi.dev)
			netif_napi_del(&q->napi);

		while (q->queued) {
			struct airoha_queue_entry_rx *e = &q->entry[q->tail];
			struct page *page = virt_to_head_page(e->buf);

			dma_sync_single_for_cpu(qdma->dev, e->dma_addr, e->dma_len,
						page_pool_get_dma_dir(q->page_pool));
			page_pool_put_full_page(q->page_pool, page, false);
			q->tail = (q->tail + 1) % q->ndesc;
			q->queued--;
		}

		if (q->page_pool)
			page_pool_destroy(q->page_pool);
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++) {
		struct airoha_tx_doneq *q = &qdma->q_tx_done[i];

		if (q->napi.dev)
			netif_napi_del(&q->napi);
	}

	if (qdma->napi_dev)
		free_netdev(qdma->napi_dev);

	return 0;
}

int en75_qdma_destroy(struct en75_qdma *qdma)
{
	guard(mutex)(&qdma->lock);
	return en75_qdma_destroy_locked(qdma);
}

int en75_qdma_use(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	guard(mutex)(&qdma->lock);

	if (WARN_ON_ONCE(qdma->destroying))
		return -EINVAL;

	if (qdma->users++ > 0)
		return 0;

	qcfg = en75_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, true);
	set_qregs_qcfg_tx_dma_en(&qcfg, true);
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_enable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_enable(&qdma->q_tx_done[i].napi);

	return 0;
}

int en75_qdma_unuse(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i, j;

	guard(mutex)(&qdma->lock);

	if (--qdma->users > 0)
		return 0;

	qcfg = en75_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, false);
	set_qregs_qcfg_tx_dma_en(&qcfg, false);
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_disable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_disable(&qdma->q_tx_done[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		struct airoha_queue_tx *q = &qdma->q_tx[i];
	
		guard(spinlock_bh)(&q->lock_bh);
		for (j = 0; j < q->ndesc; j++) {
			struct airoha_queue_entry_tx *e = &q->entry[j];

			/* In the free list already */
			if (!e->dma_addr)
				continue;

			dma_unmap_single(qdma->dev, e->dma_addr, e->dma_len,
					DMA_TO_DEVICE);
			dev_kfree_skb_any(e->skb);
			memset(e, 0, sizeof(*e));

			e->freelist_next = 0xffff;
			q->entry[q->freelist_tail].freelist_next = j;
			q->freelist_tail = j;
		}
	}

	if (qdma->destroying)
		return en75_qdma_destroy_locked(qdma);

	return 0;
}

int en75_qdma_xmit(struct en75_qdma *qdma, struct sk_buff *skb,
		   union desc_msg *msg, int qid)
{
	struct airoha_queue_tx *q = &qdma->q_tx[qid];
	struct airoha_queue_entry_tx *e;
	int len = skb_headlen(skb);
	struct desc *desc;
	dma_addr_t addr;
	u16 index;
	int ret;

	guard(spinlock_bh)(&q->lock_bh);

	index = q->freelist_head;
	if (index == 0xffff)
		return -EBUSY;

	e = &q->entry[index];
	if (e->freelist_next == 0xffff)
		return -EBUSY;

	addr = dma_map_single(qdma->dev, skb->data, len, DMA_TO_DEVICE);
	ret = dma_mapping_error(qdma->dev, addr);
	if (unlikely(ret))
		return ret;

	desc = &q->desc[index];
	WRITE_ONCE(desc->pkt_addr, addr);
	WRITE_ONCE(desc->info, (struct desc_info) { .pkt_len = len });
	WRITE_ONCE(desc->next_idx, e->freelist_next);
	for (int i = 0; i < ARRAY_SIZE(desc->msg.raw); i++)
		WRITE_ONCE(desc->msg.raw[i], msg->raw[i]);

	e->skb = skb;
	e->dma_addr = addr;
	e->dma_len = len;
	q->freelist_head = e->freelist_next;

	skb_tx_timestamp(skb);

	en75_wreg((u32)index, &q->qchain_regs->tx_cpui);

	return q->entry[e->freelist_next].freelist_next == 0xffff ?
		EBUSY : 0;

	// txq = netdev_get_tx_queue(dev, qid);
	// nr_frags = 1 + skb_shinfo(skb)->nr_frags;

	// if (airoha_dev_tx_queue_busy(q, nr_frags)) {
	// 	/* not enough space in the queue */
	// 	netif_tx_stop_queue(txq);
	// 	spin_unlock_bh(&q->lock);
	// 	return NETDEV_TX_BUSY;
	// }

	// len = skb_headlen(skb);
	// data = skb->data;
	// index = q->head;

	// for (i = 0; i < nr_frags; i++) {
	// 	struct airoha_qdma_desc *desc = &q->desc[index];
	// 	struct airoha_queue_entry *e = &q->entry[index];
	// 	skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
	// 	dma_addr_t addr;
	// 	u32 val;

	// 	addr = dma_map_single(dev->dev.parent, data, len,
	// 			      DMA_TO_DEVICE);
	// 	if (unlikely(dma_mapping_error(dev->dev.parent, addr)))
	// 		goto error_unmap;

	// 	index = (index + 1) % q->ndesc;

	// 	val = FIELD_PREP(QDMA_DESC_LEN_MASK, len);
	// 	if (i < nr_frags - 1)
	// 		val |= FIELD_PREP(QDMA_DESC_MORE_MASK, 1);
	// 	WRITE_ONCE(desc->ctrl, cpu_to_le32(val));
	// 	WRITE_ONCE(desc->addr, cpu_to_le32(addr));
	// 	val = FIELD_PREP(QDMA_DESC_NEXT_ID_MASK, index);
	// 	WRITE_ONCE(desc->data, cpu_to_le32(val));
	// 	WRITE_ONCE(desc->msg0, cpu_to_le32(msg0));
	// 	WRITE_ONCE(desc->msg1, cpu_to_le32(msg1));
	// 	WRITE_ONCE(desc->msg2, cpu_to_le32(0xffff));

	// 	e->skb = i ? NULL : skb;
	// 	e->dma_addr = addr;
	// 	e->dma_len = len;

	// 	data = skb_frag_address(frag);
	// 	len = skb_frag_size(frag);
	// }

	// q->head = index;
	// q->queued += i;

	// skb_tx_timestamp(skb);
	// netdev_tx_sent_queue(txq, skb->len);

	// if (netif_xmit_stopped(txq) || !netdev_xmit_more())
	// 	airoha_qdma_rmw(qdma, REG_TX_CPU_IDX(qid),
	// 			TX_RING_CPU_IDX_MASK,
	// 			FIELD_PREP(TX_RING_CPU_IDX_MASK, q->head));

	// if (q->ndesc - q->tqueued < q->free_thr)
	// 	return EBUSY;
	// 	// netif_tx_stop_queue(txq);

	// spin_unlock_bh(&q->lock);

	return 0;

// error_unmap:
// 	for (i--; i >= 0; i--) {
// 		index = (q->head + i) % q->ndesc;
// 		dma_unmap_single(dev->dev.parent, q->entry[index].dma_addr,
// 				 q->entry[index].dma_len, DMA_TO_DEVICE);
// 	}

	// spin_unlock_bh(&q->lock);
// error:
// 	dev_kfree_skb_any(skb);
// 	dev->stats.tx_dropped++;

// 	return NETDEV_TX_OK;
}

struct en75_qdma *en75_qdma_new(struct platform_device *pdev,
				void __iomem *qdma_regs,
				int id,
				int *irqs,
				int num_irqs,
				struct en75_qdma_cfg *cfg,
				struct en75_eth *eth)
{
	struct en75_qdma *qdma;
	int err;

	qdma = devm_kzalloc(&pdev->dev, sizeof(*qdma), GFP_KERNEL);
	if (!qdma)
		return ERR_PTR(-ENOMEM);

	qdma->dev = &pdev->dev;
	qdma->id = id;
	mutex_init(&qdma->lock);
	qdma->regs = qdma_regs;
	memcpy(&qdma->cfg, cfg, sizeof(*cfg));
	qdma->eth = eth;

	qdma->napi_dev = alloc_netdev_dummy(0);
	if (!qdma->napi_dev)
		return ERR_PTR(-ENOMEM);

	qdma->napi_dev->threaded = true;
	snprintf(qdma->napi_dev->name, sizeof qdma->napi_dev->name, "qdma%d_eth", id);

	err = en75_init(pdev, qdma, irqs, num_irqs);
	if (err) {
		en75_qdma_destroy(qdma);
		return ERR_PTR(err);
	}

	return qdma;
}

