// SPDX-License-Identifier: GPL-2.0-only
/*
 * Audio of an input: 48 kHz stereo, 16 bits, embedded in the HDMI signal.
 *
 * The card writes it into a ring of two packets of 1024 frames, raising a
 * done event as each packet completes; the poll moves the packet into a
 * software ring and notes the time and the total received. The audio of a
 * video frame is what arrived between the end of the previous frame and
 * the end of this one, the two ends placed in the audio by interpolating
 * their times between the packet events around them. A buffer is
 * therefore finished only when the first packet past its frame is in, a
 * packet period (21 ms) at most; so every frame gets its own share, 1601
 * or 1602 samples at 29.97, rather than whole packets, and no sample is
 * lost or given twice.
 *
 * In the plane the pair takes channels 0 and 1, the 16-bit sample in the
 * top bits of the 32-bit word as sdi_av.h has it; the other fourteen
 * channels are zero and audio_present says so.
 */
#include <linux/math64.h>

#include "hwsv4l2.h"

static struct hws_audio_mark *hws_mark(struct hws_chan *c, unsigned int i)
{
	return &c->marks[i];
}

/* A packet is complete. Caller holds event_lock. */
void hws_audio_done(struct hws_chan *c, u64 now_ns)
{
	const u32 *src;
	u32 toggle, pos;
	unsigned int n;

	if (!c->a_started)
		return;
	c->stat.adone++;
	/* The register names the half being written now: the other one is complete. */
	toggle = hws_rd(c->card, HWS_REG_ABUF_TOGGLE(c->index)) & 1;
	src = (const u32 *)((u8 *)c->aud_cpu + (toggle ? 0 : HWS_AUDIO_PACKET_BYTES));

	pos = c->a_total % HWS_AUDIO_RING_FRAMES;
	n = min_t(u32, HWS_AUDIO_PACKET_FRAMES, HWS_AUDIO_RING_FRAMES - pos);
	memcpy(c->aring + pos, src, n * 4);
	if (n < HWS_AUDIO_PACKET_FRAMES)
		memcpy(c->aring, src + n, (HWS_AUDIO_PACKET_FRAMES - n) * 4);
	c->a_total += HWS_AUDIO_PACKET_FRAMES;
	c->audio_packets++;

	if (c->nmarks == HWS_AUDIO_MARKS) {
		memmove(c->marks, c->marks + 1, sizeof(c->marks[0]) * (HWS_AUDIO_MARKS - 1));
		c->nmarks--;
	}
	c->marks[c->nmarks].ts = now_ns;
	c->marks[c->nmarks].end = c->a_total;
	c->nmarks++;
}

/*
 * Where in the received audio the time t falls. Between two packet
 * events the position is interpolated; before the first one known it is
 * counted back at the nominal rate; past the last one it is what has
 * arrived, since nothing later exists yet. Caller holds event_lock.
 */
static u64 hws_audio_pos(struct hws_chan *c, u64 t)
{
	struct hws_audio_mark *a, *b;
	unsigned int i;
	u64 back;

	if (!c->nmarks)
		return c->a_total;
	a = hws_mark(c, 0);
	if (t <= a->ts) {
		back = div_u64((a->ts - t) * SDI_AUDIO_RATE, NSEC_PER_SEC);
		return back < a->end ? a->end - back : 0;
	}
	for (i = 1; i < c->nmarks; i++) {
		b = hws_mark(c, i);
		if (t <= b->ts)
			return a->end + div64_u64((t - a->ts) * (b->end - a->end), b->ts - a->ts);
		a = b;
	}
	return c->a_total;
}

bool hws_audio_ready(struct hws_chan *c, u64 ts, u64 now_ns)
{
	if (!c->a_started)
		return true;
	if (c->nmarks && hws_mark(c, c->nmarks - 1)->ts >= ts)
		return true;
	return now_ns > ts + HWS_AUDIO_WAIT_NS;
}

/*
 * The audio of the frame whose done event came at ts, into the plane;
 * returns the samples per channel. Caller does not hold event_lock.
 */
unsigned int hws_audio_take(struct hws_chan *c, u64 ts, void *plane, size_t bytes)
{
	u32 *dst = plane;
	unsigned long flags;
	u64 start, end;
	unsigned int n, i;

	if (!c->a_started || !plane)
		return 0;
	spin_lock_irqsave(&c->event_lock, flags);
	end = hws_audio_pos(c, ts);
	start = c->a_frame_end;
	if (start == U64_MAX || start > end)
		start = end;
	/* What the ring no longer holds was overwritten before anyone took it. */
	if (c->a_total > HWS_AUDIO_RING_FRAMES && start < c->a_total - HWS_AUDIO_RING_FRAMES)
		start = c->a_total - HWS_AUDIO_RING_FRAMES;
	/* Past the clamp the frame's own audio may be gone entirely. */
	n = end > start ? min_t(u64, end - start, bytes / SDI_AUDIO_FRAME_BYTES) : 0;
	memset(dst, 0, (size_t)n * SDI_AUDIO_FRAME_BYTES);
	for (i = 0; i < n; i++) {
		u32 s = c->aring[(start + i) % HWS_AUDIO_RING_FRAMES];

		dst[i * SDI_AUDIO_CHANNELS] = s << 16;
		dst[i * SDI_AUDIO_CHANNELS + 1] = s & 0xffff0000;
	}
	c->a_frame_end = start + n;
	spin_unlock_irqrestore(&c->event_lock, flags);
	return n;
}

void hws_audio_start(struct hws_chan *c)
{
	struct hws_card *card = c->card;
	unsigned long flags;

	if (!c->has_audio)
		return;
	/* A done bit left from before is no packet of this run: gone before the poll may take one. */
	hws_program_window(c, c->aud_dma, HWS_REG_AUDBUF(c->index));
	hws_wr(card, HWS_REG_INT_STATUS, HWS_INT_ADONE(c->index));
	spin_lock_irqsave(&c->event_lock, flags);
	c->a_total = 0;
	c->nmarks = 0;
	c->a_frame_end = U64_MAX;
	c->audio_packets = 0;
	c->a_started = true;
	spin_unlock_irqrestore(&c->event_lock, flags);
	hws_set_bits(card, HWS_REG_ACAP_ENABLE, BIT(c->index), true);
}

void hws_audio_stop(struct hws_chan *c)
{
	unsigned long flags;

	if (!c->has_audio)
		return;
	hws_set_bits(c->card, HWS_REG_ACAP_ENABLE, BIT(c->index), false);
	spin_lock_irqsave(&c->event_lock, flags);
	c->a_started = false;
	spin_unlock_irqrestore(&c->event_lock, flags);
	hws_wr(c->card, HWS_REG_INT_STATUS, HWS_INT_ADONE(c->index));
}
