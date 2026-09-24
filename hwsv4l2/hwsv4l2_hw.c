// SPDX-License-Identifier: GPL-2.0-only
/*
 * The capture core of an HWS card: starting and stopping it, the address
 * windows of the channels, the state of the inputs.
 *
 * The start sequence is the one of the vendor's driver, kept word for word:
 * nothing documents what the individual writes do, and a core started any
 * other way was not seen to run.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "hwsv4l2.h"

void hws_set_bits(struct hws_card *card, u32 reg, u32 bits, bool on)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&card->reg_lock, flags);
	v = hws_rd(card, reg);
	v = on ? v | bits : v & ~bits;
	hws_wr(card, reg, v);
	hws_rd(card, reg);
	spin_unlock_irqrestore(&card->reg_lock, flags);
}

/*
 * Point one of the channel's buffer registers at host memory: the remap
 * entry of the channel selects the 512 MiB page, the register takes the
 * offset into it behind the channel's window. Every buffer of a channel
 * lives in the same page (hws_chan_alloc() sees to it), so rewriting the
 * entry never moves another buffer of the channel.
 */
void hws_program_window(struct hws_chan *c, dma_addr_t dma, u32 reg)
{
	struct hws_card *card = c->card;
	u32 lo = lower_32_bits(dma);

	hws_wr(card, HWS_REG_REMAP(c->index), upper_32_bits(dma));
	hws_wr(card, HWS_REG_REMAP(c->index) + 4, lo & HWS_PAGE_MASK);
	hws_wr(card, reg, (c->index + 1) * HWS_AXI_WINDOW + (lo & HWS_PAGE_OFFSET));
}

static void hws_seed_channel(struct hws_chan *c)
{
	struct hws_card *card = c->card;

	hws_program_window(c, c->slots[0].dma, HWS_REG_VBUF(c->index));
	hws_wr(card, HWS_REG_VHALF(c->index), (c->frame_bytes ? c->frame_bytes : HWS_SLOT_BYTES) / 2 / 16);
	if (c->has_audio)
		hws_program_window(c, c->aud_dma, HWS_REG_AUDBUF(c->index));
}

static int hws_wait_idle(struct hws_card *card)
{
	u32 v;

	return readl_poll_timeout(card->bar0 + HWS_REG_STATUS, v, !(v & HWS_STATUS_DMA_BUSY),
				  10, 1000000);
}

int hws_card_start(struct hws_card *card)
{
	struct pci_dev *pdev = card->pdev;
	unsigned int i;
	u32 info;

	info = hws_rd(card, HWS_REG_DEVICE_INFO);
	if (info == 0xffffffff)
		return -ENODEV;
	card->device_ver = info & 0xff;
	card->sub_ver = (info >> 8) & 0xff;

	hws_wr(card, HWS_REG_DEC_MODE, 0);
	hws_wr(card, HWS_REG_DEC_MODE, HWS_DEC_MODE_STOP);
	/* Boards past version 121 need the transfer limit; the one-input 122 does not. */
	if (card->device_ver > 121 && !(pdev->device == 0x8501 && card->device_ver == 122)) {
		hws_wr(card, HWS_REG_DMA_MAX, HWS_MAX_WIDTH * HWS_MAX_HEIGHT * 2 / 16);
		hws_rd(card, HWS_REG_DMA_MAX);
	}

	hws_wr(card, HWS_REG_DEC_MODE, 0);
	for (i = 0; i < card->nch; i++)
		if (card->ch[i])
			hws_seed_channel(card->ch[i]);
	hws_wr(card, HWS_REG_VCAP_ENABLE, 0);
	hws_wr(card, HWS_REG_ACAP_ENABLE, 0);

	hws_wr(card, HWS_REG_DEC_MODE, HWS_DEC_MODE_START);
	hws_wr(card, HWS_REG_DEC_MODE, HWS_DEC_MODE_START_ALL);
	hws_wr(card, HWS_REG_DEC_MODE, HWS_DEC_MODE_YUYV);

	/*
	 * The core's own interrupt switch is on, the bridge's enables off: that
	 * is what makes the done bits latch for the poll (hwsv4l2_module.c).
	 */
	hws_wr(card, HWS_REG_INT_STATUS, hws_rd(card, HWS_REG_INT_STATUS));
	hws_wr(card, HWS_REG_INT_DEC, 0);
	hws_wr(card, HWS_REG_BRIDGE_EN, 1);
	hws_wr(card, HWS_REG_INT_EN, 0);
	hws_set_bits(card, HWS_REG_CTL, HWS_CTL_IRQ_ENABLE, true);
	card->running = true;
	return 0;
}

void hws_card_stop(struct hws_card *card)
{
	if (hws_rd(card, HWS_REG_STATUS) == 0xffffffff)
		return;
	hws_wr(card, HWS_REG_VCAP_ENABLE, 0);
	hws_wr(card, HWS_REG_ACAP_ENABLE, 0);
	if (hws_wait_idle(card))
		dev_warn(&card->pdev->dev, "capture core still busy at stop\n");
	hws_wr(card, HWS_REG_INT_EN, 0);
	hws_wr(card, HWS_REG_INT_STATUS, hws_rd(card, HWS_REG_INT_STATUS));
	hws_wr(card, HWS_REG_DEC_MODE, HWS_DEC_MODE_STOP);
	card->running = false;
}

/*
 * Before capture starts: a card that fell off the bus is gone, a core that
 * stopped running is started again (the vendor's driver does the same).
 */
int hws_card_check(struct hws_card *card)
{
	u32 status = hws_rd(card, HWS_REG_STATUS);

	if (status == 0xffffffff)
		return -ENODEV;
	if (status & HWS_STATUS_RUNNING)
		return 0;
	dev_info(&card->pdev->dev, "capture core not running (status 0x%08x), starting it\n", status);
	return hws_card_start(card);
}

void hws_read_input(struct hws_card *card, unsigned int ch, struct hws_input *in)
{
	u32 active = hws_rd(card, HWS_REG_ACTIVE);
	u32 res = hws_rd(card, HWS_REG_IN_RES(ch));
	u32 fps = hws_rd(card, HWS_REG_IN_FPS(ch));

	memset(in, 0, sizeof(*in));
	if (active == 0xffffffff)
		return;
	in->signal = active & HWS_ACTIVE_SIGNAL(ch);
	in->interlaced = active & HWS_ACTIVE_INTERLACED(ch);
	in->hdcp = hws_rd(card, HWS_REG_HDCP) & BIT(ch);
	if (!in->signal)
		return;
	in->width = res & 0xffff;
	in->height = res >> 16;
	in->fps = fps <= 240 ? fps : 0;
}

void hws_apply_bchs(struct hws_chan *c)
{
	hws_wr(c->card, HWS_REG_BCHS(c->index),
	       c->bchs[0] | c->bchs[1] << 8 | c->bchs[2] << 16 | c->bchs[3] << 24);
}
