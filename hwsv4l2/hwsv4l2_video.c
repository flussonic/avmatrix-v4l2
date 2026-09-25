// SPDX-License-Identifier: GPL-2.0-only
/*
 * The V4L2 capture node of one HWS input.
 *
 * Frame flow. The engine of a channel writes each frame line by line to the
 * address in its buffer register, reading the register as it goes, and
 * raises a done event at the end of the frame. The next frame starts tens of
 * microseconds later, too soon to move the register between frames, so the
 * poll moves it to a fresh slot in the middle of every frame
 * (hws_slot_arm()): a frame lies in two slots, its top in the slot it started
 * in and its bottom in the next. Every slot handed out carries a marker at
 * the start of each line and in every word of a band of lines around where
 * the frame is expected to cross into it; at the done event the crossing is
 * the first line of the bottom slot whose marker the DMA overwrote, counting
 * up from the bottom, and a line the move cut in two is resolved word by
 * word inside the band. A work item copies the two parts into the client's
 * buffer (the engine cannot scatter, so the buffers are plain vmalloc
 * memory), turning YUYV into UYVY on the way when that is the format, and
 * lets the buffer wait for the first audio packet past the frame
 * (hwsv4l2_audio.c) before it hands it out.
 *
 * The frame rate the card reports is a whole number; the driver measures
 * the period itself from the done events. Until it has (two seconds of
 * capture, run by the monitor as soon as a signal appears),
 * QUERY_DV_TIMINGS answers ENOLCK.
 *
 * Frames of another raster or rate than the timings set are not delivered;
 * the queue waits, and SOURCE_CHANGE tells the client.
 */
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/vmalloc.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-dv-timings.h>

#include "hwsv4l2.h"

/*
 * One rate measurement spans two seconds: the events are dated to a quarter
 * of a millisecond, so the period comes out within 0.013%, a third of the
 * distance hws_snap_period() needs between 60 and 59.94.
 */
#define HWS_MEASURE_NS		(2ULL * NSEC_PER_SEC)
/* How long the monitor lets a rate measurement run before it gives up. */
#define HWS_PROBE_TIME		msecs_to_jiffies(3000)

#define HWS_WATCHDOG_PERIOD	msecs_to_jiffies(100)
#define HWS_STALL_NS		(500ULL * NSEC_PER_MSEC)

static const u32 hws_pixfmts[] = { SDI_PIX_FMT_UYVY, SDI_PIX_FMT_YUYV };


static inline struct hws_buffer *to_hws_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct hws_buffer, vb);
}

static bool hws_signal_change(struct hws_chan *c)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	if (!video_is_registered(&c->vdev))
		return false;
	v4l2_event_queue(&c->vdev, &ev);
	return true;
}

/* ---- geometry -------------------------------------------------------- */

static void hws_update_geometry(struct hws_chan *c)
{
	c->width = c->timings.bt.width;
	c->height = c->timings.bt.height;
	c->interlaced = c->timings.bt.interlaced;
	c->row_bytes = c->width * 2;
	c->card_row_bytes = ALIGN(c->row_bytes, 64);
	c->frame_bytes = c->card_row_bytes * c->height;
}

/* The timings of what the input carries now, if the rate is known. */
static bool hws_detected_timings(struct hws_chan *c, struct v4l2_dv_timings *t)
{
	u32 w, h;
	bool il;

	if (!c->in.signal || !c->period_ns)
		return false;
	hws_frame_of_input(&c->in, &w, &h, &il);
	if (!w || !h)
		return false;
	hws_timings_for(w, h, il, c->period_ns, t);
	return true;
}

/* The input carries the raster and rate of the timings set. */
static bool hws_input_matches(struct hws_chan *c)
{
	u64 p = hws_timings_period(&c->timings);
	u32 w, h;
	bool il;

	if (!c->in.signal)
		return false;
	hws_frame_of_input(&c->in, &w, &h, &il);
	if (w != c->width || h != c->height || il != c->interlaced)
		return false;
	if (!c->period_ns || !p)
		return true;
	/* Within 0.05%: half the distance between 60 and 59.94. */
	return (p > c->period_ns ? p - c->period_ns : c->period_ns - p) * 2000 <= p;
}

/* ---- slots ----------------------------------------------------------- */

/*
 * The marker at the start of every line of a slot handed out, and in every
 * word of the lines around where the frame is expected to cross into it:
 * black and white with the colour difference at its extremes, which the
 * card does not produce.
 */
#define HWS_MARK_A		0x00ff00ffu
#define HWS_MARK_B		0xff00ff00u
/* Lines fully marked around the expected crossing: before it, and after. */
#define HWS_BAND_BEFORE		32
#define HWS_BAND_AFTER		96

static u32 *hws_line(struct hws_chan *c, struct hws_slot *s, u32 y)
{
	return (u32 *)((u8 *)s->cpu + (size_t)y * c->card_row_bytes);
}

static bool hws_line_marked(struct hws_chan *c, struct hws_slot *s, u32 y)
{
	const u32 *p = hws_line(c, s, y);

	return READ_ONCE(p[0]) == HWS_MARK_A && READ_ONCE(p[1]) == HWS_MARK_B;
}

/* Whether the last word of a line still holds what the marker put there. */
static bool hws_line_end_marked(struct hws_chan *c, struct hws_slot *s, u32 y)
{
	const u32 *p = hws_line(c, s, y);

	return READ_ONCE(p[c->row_bytes / 4 - 1]) == HWS_MARK_B;
}

/*
 * Mark a slot about to be handed out. The engine is at line `at` of the
 * frame it is writing (the height when it is not known): the lines around it
 * are marked whole, so that the line the engine is in when the register
 * moves shows where in it the move happened.
 */
static void hws_slot_mark(struct hws_chan *c, struct hws_slot *s, u32 at)
{
	u32 y, words = c->row_bytes / 4;

	s->mark_at = at;
	s->band_lo = at > HWS_BAND_BEFORE ? at - HWS_BAND_BEFORE : 0;
	s->band_hi = min(at + HWS_BAND_AFTER, c->height);
	for (y = 0; y < c->height; y++) {
		u32 *p = hws_line(c, s, y);

		if (y >= s->band_lo && y < s->band_hi) {
			memset32(p, HWS_MARK_B, words);
		} else {
			p[1] = HWS_MARK_B;
			p[words - 1] = HWS_MARK_B;
		}
		p[0] = HWS_MARK_A;
	}
}

/* The first word of a line of the band the DMA wrote, the line's words if none. */
static u32 hws_first_written(struct hws_chan *c, struct hws_slot *s, u32 y)
{
	const u32 *p = hws_line(c, s, y);
	u32 i, words = c->row_bytes / 4;

	for (i = 2; i < words; i++)
		if (READ_ONCE(p[i]) != HWS_MARK_B)
			return i;
	return words;
}

static void hws_slot_put(struct hws_slot *s)
{
	if (s && s->users)
		s->users--;
}

static void hws_frame_remove(struct hws_chan *c, unsigned int i)
{
	hws_slot_put(c->frames[i].top);
	hws_slot_put(c->frames[i].bottom);
	c->nframes--;
	memmove(c->frames + i, c->frames + i + 1, sizeof(c->frames[0]) * (c->nframes - i));
}

/*
 * Give up the oldest frame waiting for its copy, the one being copied
 * excepted. Caller holds event_lock.
 */
static bool hws_drop_oldest(struct hws_chan *c)
{
	unsigned int i = c->nframes && c->frames[0].copying ? 1 : 0;

	if (i >= c->nframes)
		return false;
	hws_frame_remove(c, i);
	c->stat.skipped++;
	if (list_empty(&c->queued))
		c->stat.no_buffer++;
	return true;
}

/*
 * Whether a slot marked ahead of time serves a crossing at line `at`: the
 * band reaches HWS_BAND_BEFORE lines above the line it was laid out around
 * and HWS_BAND_AFTER below, and a crossing a little off that line still
 * falls well inside it.
 */
static bool hws_slot_fits(const struct hws_slot *s, u32 at)
{
	return s->ready && at + HWS_BAND_BEFORE / 2 >= s->mark_at &&
	       at <= s->mark_at + HWS_BAND_AFTER / 2;
}

/* A free slot, one marked for this crossing if there is one. */
static struct hws_slot *hws_free_slot(struct hws_chan *c, u32 at)
{
	struct hws_slot *any = NULL;
	unsigned int i;

	for (i = 0; i < HWS_NUM_SLOTS; i++) {
		struct hws_slot *s = &c->slots[i];

		if (s->users || s->marking)
			continue;
		if (hws_slot_fits(s, at))
			return s;
		if (!any)
			any = s;
	}
	return any;
}

/*
 * Move the buffer register to a fresh slot in the middle of the frame the
 * engine is writing: its lines from here on go to the new slot, and so does
 * the top of the next frame. The slot is normally marked already
 * (hws_video_premark()); one that is not, or not for this line, is marked
 * here. Caller holds event_lock.
 */
static void hws_slot_arm(struct hws_chan *c, u64 now_ns)
{
	struct hws_slot *pick;
	u32 at = c->height;

	if (c->cur.top && c->period_ns && c->last_done_ns && now_ns > c->last_done_ns)
		at = min_t(u64, div64_u64((now_ns - c->last_done_ns) * c->height, c->period_ns),
			   c->height);
	for (;;) {
		pick = hws_free_slot(c, at);
		if (pick || !hws_drop_oldest(c))
			break;
	}
	if (!pick)
		return;
	if (!hws_slot_fits(pick, at))
		hws_slot_mark(c, pick, at);
	pick->ready = false;
	pick->users++;			/* the register */
	/* The markers are in memory before the engine may write the slot. */
	wmb();
	hws_program_window(c, pick->dma, HWS_REG_VBUF(c->index));
	hws_slot_put(c->base);
	c->base = pick;
	if (c->cur.top && !c->cur.bottom) {
		c->cur.bottom = pick;
		pick->users++;		/* the frame's bottom */
	}
}

/* Every poll: move the register once the engine is well into the frame. */
void hws_video_poll(struct hws_chan *c, u64 now_ns)
{
	if (c->vcap_on && c->streaming && c->arm_pending && now_ns >= c->arm_at) {
		c->arm_pending = false;
		hws_slot_arm(c, now_ns);
	}
}

/*
 * Mark the free slots ahead of time, in process context: marking is a pass
 * over every line of a slot and a whole band, which the poll's hardirq
 * should not carry. The band is laid out around the line the frame crosses
 * at when the poll is on time, halfway down once the period is known.
 */
static void hws_video_premark(struct hws_chan *c)
{
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < HWS_NUM_SLOTS; i++) {
		struct hws_slot *s = &c->slots[i];
		u32 at;

		spin_lock_irqsave(&c->event_lock, flags);
		if (!c->streaming || s->users || s->marking || s->ready) {
			spin_unlock_irqrestore(&c->event_lock, flags);
			continue;
		}
		s->marking = true;
		at = c->period_ns ? c->height / 2 : c->height;
		spin_unlock_irqrestore(&c->event_lock, flags);

		hws_slot_mark(c, s, at);

		spin_lock_irqsave(&c->event_lock, flags);
		s->marking = false;
		s->ready = true;
		spin_unlock_irqrestore(&c->event_lock, flags);
	}
}

/*
 * Every slot free again, the frames waiting for their copy given up -- all
 * but one the work item is copying now, which keeps its slots until the
 * copy is over. Caller holds event_lock.
 */
static void hws_slots_reset(struct hws_chan *c)
{
	bool keep = c->nframes && c->frames[0].copying;
	unsigned int i;

	for (i = 0; i < HWS_NUM_SLOTS; i++) {
		c->slots[i].users = 0;
		c->slots[i].ready = false;
	}
	c->base = NULL;
	memset(&c->cur, 0, sizeof(c->cur));
	c->nframes = 0;
	if (keep) {
		c->nframes = 1;
		c->frames[0].top->users++;
		c->frames[0].bottom->users++;
	}
}

/* ---- capture engine -------------------------------------------------- */

static void hws_vcap_start(struct hws_chan *c)
{
	struct hws_card *card = c->card;
	unsigned long flags;
	bool was_on;

	hws_wr(card, HWS_REG_OUT_RES(c->index), c->height << 16 | c->width);
	hws_wr(card, HWS_REG_VHALF(c->index), c->frame_bytes / 2 / 16);
	spin_lock_irqsave(&c->event_lock, flags);
	was_on = c->vcap_on;
	hws_slots_reset(c);
	/* The frame running now is joined halfway; the first whole one follows it. */
	hws_slot_arm(c, 0);
	c->arm_pending = false;
	c->vcap_on = true;
	c->last_done_ns = 0;
	c->m_frames = 0;
	spin_unlock_irqrestore(&c->event_lock, flags);
	hws_wr(card, HWS_REG_INT_STATUS, HWS_INT_VDONE(c->index));
	hws_set_bits(card, HWS_REG_VCAP_ENABLE, BIT(c->index), true);
	/* One hold on the poll per running engine, however often it is started. */
	if (!was_on)
		hws_poll_get(card);
}

static void hws_vcap_stop(struct hws_chan *c)
{
	unsigned long flags;
	bool was_on;

	hws_set_bits(c->card, HWS_REG_VCAP_ENABLE, BIT(c->index), false);
	spin_lock_irqsave(&c->event_lock, flags);
	was_on = c->vcap_on;
	c->vcap_on = false;
	spin_unlock_irqrestore(&c->event_lock, flags);
	hws_wr(c->card, HWS_REG_INT_STATUS, HWS_INT_VDONE(c->index));
	if (was_on)
		hws_poll_put(c->card);
}

/*
 * The frame period from the done events, a measurement two seconds long; a
 * gap in the events starts it over. Caller holds event_lock.
 */
static void hws_measure(struct hws_chan *c, u64 now_ns)
{
	u64 measured, period, gap = c->period_ns;
	bool known;

	/* Before the period is known the gap is judged by the running estimate. */
	if (!gap && c->m_frames > 1)
		gap = div_u64(c->m_last_ns - c->m_first_ns, c->m_frames - 1);
	if (c->m_frames && gap && (s64)(now_ns - c->m_last_ns) > (s64)(gap * 3 / 2))
		c->m_frames = 0;
	if (!c->m_frames) {
		c->m_first_ns = now_ns;
		c->m_last_ns = now_ns;
		c->m_frames = 1;
		return;
	}
	c->m_last_ns = now_ns;
	c->m_frames++;
	if (now_ns - c->m_first_ns < HWS_MEASURE_NS)
		return;
	measured = div_u64(c->m_last_ns - c->m_first_ns, c->m_frames - 1);
	c->m_first_ns = now_ns;
	c->m_frames = 1;
	period = hws_snap_period(measured, &known);
	if (c->period_ns &&
	    (period > c->period_ns ? period - c->period_ns : c->period_ns - period) * 2000 <= c->period_ns)
		return;
	if (c->period_ns)
		dev_info(&c->card->pdev->dev, "input %u: frame period %llu ns, was %llu\n",
			 c->index + 1, period, c->period_ns);
	c->period_ns = period;
	if (c->streaming)
		hws_signal_change(c);
}

/*
 * A frame is done. When the register moved during it, the frame lies in the
 * slot it started in and the one the register moved to, and the line where
 * it crossed is the first of the bottom slot whose marker the DMA
 * overwrote, counting up from the bottom: the next frame is already filling
 * that slot from its first line, but it reaches the crossing only half a
 * frame later. When the register did not move -- the poll that moves it
 * came late -- the next frame is being written over this one, and this one
 * is given up. Caller holds event_lock.
 */
void hws_video_done(struct hws_chan *c, u64 now_ns)
{
	struct hws_frame *f = &c->cur;
	u32 y;

	c->stat.vdone++;
	if (!c->vcap_on)
		return;

	/* Signed: the watchdog dates its restart from a clock read of its own. */
	if (c->streaming && c->last_done_ns && c->period_ns &&
	    (s64)(now_ns - c->last_done_ns) > (s64)(c->period_ns * 3 / 2)) {
		u32 missed = div64_u64(now_ns - c->last_done_ns + c->period_ns / 2, c->period_ns) - 1;

		c->stat.events_missed += missed;
		c->stat.skipped += missed;
		c->sequence += missed;
	}
	c->last_done_ns = now_ns;
	hws_measure(c, now_ns);
	if (!c->streaming)
		return;

	if (f->top && f->bottom) {
		for (y = c->height; y > 0 && !hws_line_marked(c, f->bottom, y - 1); y--)
			;
		f->cut = y;
		f->split_word = 0;
		f->split_lost = false;
		/* The line above the cut: the move may have come in the middle of it. */
		if (y && y < c->height && !hws_line_end_marked(c, f->bottom, y - 1)) {
			if (y - 1 >= f->bottom->band_lo && y - 1 < f->bottom->band_hi)
				f->split_word = hws_first_written(c, f->bottom, y - 1);
			else
				f->split_lost = true;
		}
		f->ts = now_ns;
		f->sequence = c->sequence;
		if (c->nframes == HWS_NUM_FRAMES && !hws_drop_oldest(c)) {
			hws_slot_put(f->top);
			hws_slot_put(f->bottom);
			c->stat.skipped++;
		} else {
			c->frames[c->nframes++] = *f;
		}
		queue_work(c->card->wq, &c->done_work);
	} else if (f->top) {
		hws_slot_put(f->top);
		c->stat.resyncs++;
		c->stat.skipped++;
	}
	c->sequence++;

	/* The next frame starts in the slot in the register. */
	memset(f, 0, sizeof(*f));
	if (c->base) {
		f->top = c->base;
		c->base->users++;
	}
	c->arm_pending = true;
	c->arm_at = now_ns + (c->period_ns ? c->period_ns / 2 : 8 * NSEC_PER_MSEC);
}

/* ---- buffer completion ----------------------------------------------- */

/*
 * Words [from, to) of line y of a slot into the plane, as YUYV or turned
 * into UYVY: Y0 Cb Y1 Cr -> Cb Y0 Cr Y1, the bytes of every 16-bit word
 * swapped.
 */
static void hws_copy_words(struct hws_chan *c, struct hws_slot *slot, u32 y, u32 from, u32 to,
			   void *dst)
{
	const u32 *s = (const u32 *)hws_line(c, slot, y);
	u32 *d = (u32 *)((u8 *)dst + (size_t)y * c->row_bytes);
	u32 i = from;

	if (c->pixfmt == SDI_PIX_FMT_YUYV) {
		memcpy(d + from, s + from, (to - from) * 4);
		return;
	}
	for (; i < to && (i & 1); i++)
		d[i] = ((s[i] & 0x00ff00ff) << 8) | ((s[i] >> 8) & 0x00ff00ff);
	/* Eight bytes at a time; memcpy, since a line of 4n+2 pixels leaves them unaligned. */
	for (; i + 2 <= to; i += 2) {
		u64 v;

		memcpy(&v, s + i, sizeof(v));
		v = ((v & 0x00ff00ff00ff00ffULL) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffULL);
		memcpy(d + i, &v, sizeof(v));
	}
	for (; i < to; i++)
		d[i] = ((s[i] & 0x00ff00ff) << 8) | ((s[i] >> 8) & 0x00ff00ff);
}

static void hws_copy_lines(struct hws_chan *c, struct hws_slot *slot, u32 from, u32 to, void *dst)
{
	u32 y;

	for (y = from; y < to; y++)
		hws_copy_words(c, slot, y, 0, c->row_bytes / 4, dst);
}

/*
 * SMPTE 337M / IEC 61937 data bursts start with the preamble Pa Pb in the
 * two channels of the pair; the 16-bit form is the only one HDMI carries.
 */
static u32 hws_audio_nonpcm(const u32 *s, unsigned int samples)
{
	unsigned int i;

	for (i = 0; i < samples; i++, s += SDI_AUDIO_CHANNELS)
		if (s[0] >> 16 == 0xf872 && s[1] >> 16 == 0x4e1f)
			return BIT(0) | BIT(1);
	return 0;
}

static void hws_finish_buffer(struct hws_chan *c, struct hws_buffer *buf)
{
	struct vb2_buffer *vb = &buf->vb.vb2_buf;
	void *audio = vb2_plane_vaddr(vb, SDI_PLANE_AUDIO);
	struct sdi_meta *meta = vb2_plane_vaddr(vb, SDI_PLANE_META);
	struct hwav_meta *hw = (struct hwav_meta *)((u8 *)meta + SDI_META_SIZE);
	unsigned int samples;

	samples = hws_audio_take(c, buf->ts, audio, vb2_plane_size(vb, SDI_PLANE_AUDIO));
	vb2_set_plane_payload(vb, SDI_PLANE_AUDIO, (size_t)samples * SDI_AUDIO_FRAME_BYTES);
	vb2_set_plane_payload(vb, SDI_PLANE_ANC, 0);
	vb2_set_plane_payload(vb, SDI_PLANE_VBI, 0);

	memset(meta, 0, HWAV_META_BYTES);
	meta->magic = SDI_META_MAGIC;
	meta->version = SDI_META_VERSION;
	if (c->a_started) {
		meta->audio_present = samples ? BIT(0) | BIT(1) : 0;
		meta->audio_samples[0] = samples;
		meta->audio_rate = SDI_AUDIO_RATE;
		if (samples)
			meta->audio_nonpcm = hws_audio_nonpcm(audio, samples);
	}
	meta->vendor_bytes = sizeof(*hw);
	meta->vendor_magic = HWAV_VENDOR_MAGIC;
	meta->vendor_version = HWAV_VENDOR_VERSION;
	if (c->in.hdcp)
		hw->flags |= HWAV_F_HDCP;
	if (c->in.width != c->width || (c->in.interlaced ? c->in.height * 2 : c->in.height) != c->height)
		hw->flags |= HWAV_F_SCALED;
	hw->in_width = c->in.width;
	hw->in_height = c->in.height;
	hw->in_fps = c->in.fps;
	hw->frame_period_ns = buf->period_ns;
	hw->audio_packets = c->audio_packets;
	vb2_set_plane_payload(vb, SDI_PLANE_META, HWAV_META_BYTES);

	c->stat.frames++;
	vb2_buffer_done(vb, buf->bad ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
}

/*
 * Copy every complete frame into a queued buffer, then hand out the buffers
 * whose audio is in. The bottom of a frame is copied first: the next frame
 * is filling its slot from the first line and reaches the cut half a frame
 * after the done event; the line above the cut still carrying its marker
 * when the copy is over says it did not get there.
 */
static void hws_done_work(struct work_struct *w)
{
	struct hws_chan *c = container_of(w, struct hws_chan, done_work);
	unsigned long flags;

	for (;;) {
		struct hws_buffer *buf = NULL;
		struct hws_frame f;
		void *dst;
		bool bad;

		spin_lock_irqsave(&c->event_lock, flags);
		if (!c->nframes || !c->streaming) {
			spin_unlock_irqrestore(&c->event_lock, flags);
			break;
		}
		f = c->frames[0];
		if (!hws_input_matches(c)) {
			c->stat.no_sync++;
			c->stat.skipped++;
		} else if (list_empty(&c->queued)) {
			c->stat.no_buffer++;
			c->stat.skipped++;
		} else {
			buf = list_first_entry(&c->queued, struct hws_buffer, list);
			list_del_init(&buf->list);
		}
		/* The frame stays at the head, its slots held, until the copy is over. */
		if (buf)
			c->frames[0].copying = true;
		else
			hws_frame_remove(c, 0);
		spin_unlock_irqrestore(&c->event_lock, flags);
		if (!buf)
			continue;

		dst = vb2_plane_vaddr(&buf->vb.vb2_buf, SDI_PLANE_VIDEO);
		hws_copy_lines(c, f.bottom, f.cut, c->height, dst);
		if (f.split_word)
			hws_copy_words(c, f.bottom, f.cut - 1, f.split_word, c->row_bytes / 4, dst);
		/* The copy is read before the marker that says it was still whole. */
		rmb();
		bad = !f.cut || f.split_lost ||
		      (f.cut < c->height && !hws_line_marked(c, f.bottom, f.cut - 1));
		hws_copy_lines(c, f.top, 0, f.split_word ? f.cut - 1 : f.cut, dst);
		if (f.split_word)
			hws_copy_words(c, f.top, f.cut - 1, 0, f.split_word, dst);
		if (f.split_word)
			c->stat.split_lines++;
		vb2_set_plane_payload(&buf->vb.vb2_buf, SDI_PLANE_VIDEO,
				      (size_t)c->row_bytes * c->height);
		buf->ts = f.ts;
		buf->period_ns = c->period_ns;
		buf->vb.sequence = f.sequence;
		buf->vb.field = c->interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
		buf->vb.vb2_buf.timestamp = f.ts;
		buf->bad = bad;

		spin_lock_irqsave(&c->event_lock, flags);
		if (bad)
			c->stat.dma_errors++;
		/* stop_streaming may have emptied the queue meanwhile. */
		if (c->nframes && c->frames[0].copying)
			hws_frame_remove(c, 0);
		list_add_tail(&buf->list, &c->waiting);
		spin_unlock_irqrestore(&c->event_lock, flags);
	}

	for (;;) {
		struct hws_buffer *buf;

		spin_lock_irqsave(&c->event_lock, flags);
		buf = c->streaming ? list_first_entry_or_null(&c->waiting, struct hws_buffer, list) : NULL;
		if (buf && hws_audio_ready(c, buf->ts, ktime_get_ns()))
			list_del_init(&buf->list);
		else
			buf = NULL;
		spin_unlock_irqrestore(&c->event_lock, flags);
		if (!buf)
			break;
		hws_finish_buffer(c, buf);
	}
	hws_video_premark(c);
}

/* ---- watchdog -------------------------------------------------------- */

/*
 * While streaming: finishes buffers whose audio never came, and restarts
 * the engine when a signal is there and no frame has been done for half a
 * second.
 */
/*
 * Restart a stalled engine; caller holds the queue lock, so STREAMOFF is
 * not halfway through. The frames waiting for their copy are given up
 * (hws_slots_reset()).
 */
static void hws_restart(struct hws_chan *c)
{
	unsigned long flags;

	dev_warn_ratelimited(&c->card->pdev->dev, "input %u: no frame done, capture restarted\n",
			     c->index + 1);
	hws_vcap_stop(c);
	spin_lock_irqsave(&c->event_lock, flags);
	c->stat.skipped += c->nframes - (c->nframes && c->frames[0].copying ? 1 : 0);
	c->stat.restarts++;
	spin_unlock_irqrestore(&c->event_lock, flags);
	hws_vcap_start(c);
	spin_lock_irqsave(&c->event_lock, flags);
	c->last_done_ns = ktime_get_ns();
	spin_unlock_irqrestore(&c->event_lock, flags);
}

static void hws_watchdog(struct work_struct *w)
{
	struct hws_chan *c = container_of(to_delayed_work(w), struct hws_chan, watchdog);
	unsigned long flags;
	bool streaming, stalled;

	spin_lock_irqsave(&c->event_lock, flags);
	streaming = c->streaming;
	stalled = streaming && c->in.signal && c->last_done_ns &&
		  (s64)(ktime_get_ns() - c->last_done_ns) > (s64)HWS_STALL_NS;
	spin_unlock_irqrestore(&c->event_lock, flags);
	if (!streaming)
		return;
	/*
	 * STREAMOFF holds the queue lock while it cancels this work, so the lock
	 * is only tried; a stall still there is seen again next period.
	 */
	if (stalled && mutex_trylock(&c->lock)) {
		if (c->streaming)
			hws_restart(c);
		mutex_unlock(&c->lock);
	}
	queue_work(c->card->wq, &c->done_work);
	schedule_delayed_work(&c->watchdog, HWS_WATCHDOG_PERIOD);
}

/* ---- the input, watched by the card's monitor ------------------------ */

/*
 * The monitor read a new state of the input. A change of signal or raster
 * forgets the measured rate and tells the client; the rate is measured
 * again as soon as there is a signal.
 */
void hws_video_input_changed(struct hws_chan *c)
{
	struct hws_input in;
	unsigned long flags;
	bool changed;

	hws_read_input(c->card, c->index, &in);
	spin_lock_irqsave(&c->event_lock, flags);
	changed = in.signal != c->in.signal || in.width != c->in.width ||
		  in.height != c->in.height || in.interlaced != c->in.interlaced;
	c->in = in;
	if (changed) {
		c->period_ns = 0;
		c->m_frames = 0;
	}
	spin_unlock_irqrestore(&c->event_lock, flags);
	if (!changed)
		return;
	if (in.signal)
		dev_info(&c->card->pdev->dev, "input %u: %ux%u%s signal%s\n", c->index + 1,
			 in.width, in.height, in.interlaced ? " interlaced" : "",
			 in.hdcp ? ", HDCP" : "");
	else
		dev_info(&c->card->pdev->dev, "input %u: no signal\n", c->index + 1);
	hws_signal_change(c);
}

/*
 * The rate of an input nobody captures is measured by running its engine
 * for a moment into the driver's own slots.
 */
void hws_video_probe_tick(struct hws_chan *c)
{
	bool start = false, stop = false;

	mutex_lock(&c->lock);
	if (!c->streaming) {
		if (c->probing && (c->period_ns || time_after(jiffies, c->probe_until) || !c->in.signal))
			stop = true;
		else if (!c->probing && c->in.signal && !c->period_ns &&
			 time_after(jiffies, c->probe_until))
			start = true;
	}
	if (stop) {
		hws_vcap_stop(c);
		c->probing = false;
		/* A failed measurement is tried again after a pause as long. */
		if (!c->period_ns)
			c->probe_until = jiffies + HWS_PROBE_TIME;
	}
	if (start) {
		u32 w, h;
		bool il;

		/* Measured in the input's own raster, so that the frame fits the slots. */
		hws_frame_of_input(&c->in, &w, &h, &il);
		if (w && h) {
			struct v4l2_dv_timings saved = c->timings;

			hws_timings_for(w, h, il, 0, &c->timings);
			hws_update_geometry(c);
			c->probing = true;
			c->probe_until = jiffies + HWS_PROBE_TIME;
			hws_vcap_start(c);
			c->timings = saved;
			hws_update_geometry(c);
		}
	}
	mutex_unlock(&c->lock);
}

/* ---- vb2 ------------------------------------------------------------- */

static int hws_queue_setup(struct vb2_queue *q, unsigned int *nbuffers, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct hws_chan *c = vb2_get_drv_priv(q);
	size_t video = (size_t)c->row_bytes * c->height;

	BUILD_BUG_ON(sizeof(struct sdi_meta) != SDI_META_SIZE);
	if (*nplanes) {
		if (*nplanes != SDI_NUM_PLANES || sizes[SDI_PLANE_VIDEO] < video ||
		    sizes[SDI_PLANE_AUDIO] < SDI_AUDIO_PLANE_SIZE ||
		    sizes[SDI_PLANE_META] < HWAV_META_BYTES)
			return -EINVAL;
		return 0;
	}
	*nplanes = SDI_NUM_PLANES;
	sizes[SDI_PLANE_VIDEO] = video;
	sizes[SDI_PLANE_AUDIO] = SDI_AUDIO_PLANE_SIZE;
	sizes[SDI_PLANE_ANC] = SDI_ANC_PLANE_SIZE;
	sizes[SDI_PLANE_META] = PAGE_ALIGN(HWAV_META_BYTES);
	sizes[SDI_PLANE_VBI] = SDI_VBI_PLANE_SIZE;
	return 0;
}

static int hws_buf_prepare(struct vb2_buffer *vb)
{
	struct hws_chan *c = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, SDI_PLANE_VIDEO) < (size_t)c->row_bytes * c->height ||
	    vb2_plane_size(vb, SDI_PLANE_META) < HWAV_META_BYTES)
		return -EINVAL;
	return 0;
}

static void hws_buf_queue(struct vb2_buffer *vb)
{
	struct hws_chan *c = vb2_get_drv_priv(vb->vb2_queue);
	struct hws_buffer *buf = to_hws_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&c->event_lock, flags);
	list_add_tail(&buf->list, &c->queued);
	spin_unlock_irqrestore(&c->event_lock, flags);
}

static void hws_return_buffers(struct hws_chan *c, enum vb2_buffer_state state)
{
	struct hws_buffer *buf, *tmp;
	unsigned long flags;
	LIST_HEAD(all);

	spin_lock_irqsave(&c->event_lock, flags);
	list_splice_tail_init(&c->queued, &all);
	list_splice_tail_init(&c->waiting, &all);
	spin_unlock_irqrestore(&c->event_lock, flags);
	list_for_each_entry_safe(buf, tmp, &all, list) {
		list_del_init(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static int hws_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hws_chan *c = vb2_get_drv_priv(q);
	unsigned long flags;
	int ret;

	ret = hws_card_check(c->card);
	if (ret) {
		hws_return_buffers(c, VB2_BUF_STATE_QUEUED);
		return ret;
	}
	if (c->probing) {
		hws_vcap_stop(c);
		c->probing = false;
	}
	spin_lock_irqsave(&c->event_lock, flags);
	memset(&c->stat, 0, sizeof(c->stat));
	c->sequence = 0;
	c->streaming = true;
	spin_unlock_irqrestore(&c->event_lock, flags);

	hws_audio_start(c);
	hws_vcap_start(c);
	hws_video_premark(c);
	schedule_delayed_work(&c->watchdog, HWS_WATCHDOG_PERIOD);
	dev_info(&c->card->pdev->dev, "input %u: capture %ux%u%c, period %llu ns, %p4cc\n",
		 c->index + 1, c->width, c->height, c->interlaced ? 'i' : 'p',
		 hws_timings_period(&c->timings), &c->pixfmt);
	return 0;
}

static void hws_stop_streaming(struct vb2_queue *q)
{
	struct hws_chan *c = vb2_get_drv_priv(q);
	unsigned long flags;

	hws_vcap_stop(c);
	hws_audio_stop(c);
	spin_lock_irqsave(&c->event_lock, flags);
	c->streaming = false;
	spin_unlock_irqrestore(&c->event_lock, flags);
	cancel_delayed_work_sync(&c->watchdog);
	cancel_work_sync(&c->done_work);
	spin_lock_irqsave(&c->event_lock, flags);
	hws_slots_reset(c);
	spin_unlock_irqrestore(&c->event_lock, flags);
	hws_return_buffers(c, VB2_BUF_STATE_ERROR);
	dev_info(&c->card->pdev->dev,
		 "input %u: stop: frames %llu skipped %llu no-buffer %llu no-sync %llu resyncs %llu missed %llu dma errors %llu restarts %llu (vdone %u adone %u split lines %u)\n",
		 c->index + 1, c->stat.frames, c->stat.skipped, c->stat.no_buffer, c->stat.no_sync,
		 c->stat.resyncs, c->stat.events_missed, c->stat.dma_errors, c->stat.restarts, c->stat.vdone,
		 c->stat.adone, c->stat.split_lines);
}

static const struct vb2_ops hws_vb2_ops = {
	.queue_setup = hws_queue_setup,
	.buf_prepare = hws_buf_prepare,
	.buf_queue = hws_buf_queue,
	.start_streaming = hws_start_streaming,
	.stop_streaming = hws_stop_streaming,
	HWS_VB2_WAIT_OPS
};

/* ---- ioctls ---------------------------------------------------------- */

static const char *hws_kind(const struct hws_chan *c)
{
	return c->card->sdi ? "SDI" : "HDMI";
}

static int hws_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct hws_chan *c = video_drvdata(file);

	strscpy(cap->driver, HWS_DRV_NAME, sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "%s %s in %u", c->card->short_name, hws_kind(c),
		 c->index + 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", pci_name(c->card->pdev));
	return 0;
}

static int hws_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	static const char *const names[] = { "HWS UYVY 4:2:2 8-bit", "HWS YUYV 4:2:2 8-bit" };

	if (f->index >= ARRAY_SIZE(hws_pixfmts))
		return -EINVAL;
	f->pixelformat = hws_pixfmts[f->index];
	strscpy(f->description, names[f->index], sizeof(f->description));
	return 0;
}

static u32 hws_pixfmt_or_default(u32 want)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hws_pixfmts); i++)
		if (hws_pixfmts[i] == want)
			return want;
	return hws_pixfmts[0];
}

static void hws_fill_fmt(struct hws_chan *c, u32 pixfmt, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	memset(pix, 0, sizeof(*pix));
	pix->pixelformat = pixfmt;
	pix->width = c->width;
	pix->height = c->height;
	pix->field = c->interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	pix->colorspace = c->height <= 576 ? V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	pix->num_planes = SDI_NUM_PLANES;
	pix->plane_fmt[SDI_PLANE_VIDEO].bytesperline = c->row_bytes;
	pix->plane_fmt[SDI_PLANE_VIDEO].sizeimage = c->row_bytes * c->height;
	pix->plane_fmt[SDI_PLANE_AUDIO].sizeimage = SDI_AUDIO_PLANE_SIZE;
	pix->plane_fmt[SDI_PLANE_ANC].sizeimage = SDI_ANC_PLANE_SIZE;
	pix->plane_fmt[SDI_PLANE_META].sizeimage = PAGE_ALIGN(HWAV_META_BYTES);
	pix->plane_fmt[SDI_PLANE_VBI].sizeimage = SDI_VBI_PLANE_SIZE;
}

static int hws_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hws_chan *c = video_drvdata(file);

	hws_fill_fmt(c, c->pixfmt, f);
	return 0;
}

static int hws_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hws_chan *c = video_drvdata(file);

	hws_fill_fmt(c, hws_pixfmt_or_default(f->fmt.pix_mp.pixelformat), f);
	return 0;
}

static int hws_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hws_chan *c = video_drvdata(file);

	if (vb2_is_busy(&c->queue))
		return -EBUSY;
	c->pixfmt = hws_pixfmt_or_default(f->fmt.pix_mp.pixelformat);
	hws_fill_fmt(c, c->pixfmt, f);
	return 0;
}

static int hws_enum_input(struct file *file, void *fh, struct v4l2_input *inp)
{
	struct hws_chan *c = video_drvdata(file);
	struct hws_input in;

	if (inp->index)
		return -EINVAL;
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	snprintf(inp->name, sizeof(inp->name), "%s %u", hws_kind(c), c->index + 1);
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	hws_read_input(c->card, c->index, &in);
	if (!in.signal)
		inp->status = V4L2_IN_ST_NO_SIGNAL;
	else if (in.hdcp)
		inp->status = V4L2_IN_ST_NO_ACCESS;
	return 0;
}

static int hws_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int hws_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int hws_query_dv_timings(struct file *file, void *fh, struct v4l2_dv_timings *t)
{
	struct hws_chan *c = video_drvdata(file);
	unsigned long flags;
	bool ok;

	spin_lock_irqsave(&c->event_lock, flags);
	ok = hws_detected_timings(c, t);
	spin_unlock_irqrestore(&c->event_lock, flags);
	if (ok)
		return 0;
	return c->in.signal ? -ENOLCK : -ENOLINK;
}

static int hws_s_dv_timings(struct file *file, void *fh, struct v4l2_dv_timings *t)
{
	struct hws_chan *c = video_drvdata(file);

	if (!hws_timings_fit(t))
		return -ERANGE;
	if (v4l2_match_dv_timings(t, &c->timings, 0, true)) {
		*t = c->timings;
		return 0;
	}
	if (vb2_is_busy(&c->queue))
		return -EBUSY;
	c->timings = *t;
	hws_update_geometry(c);
	return 0;
}

static int hws_g_dv_timings(struct file *file, void *fh, struct v4l2_dv_timings *t)
{
	struct hws_chan *c = video_drvdata(file);

	*t = c->timings;
	return 0;
}

static int hws_enum_dv_timings(struct file *file, void *fh, struct v4l2_enum_dv_timings *e)
{
	return v4l2_enum_dv_timings_cap(e, &hws_timings_cap, NULL, NULL);
}

static int hws_dv_timings_cap(struct file *file, void *fh, struct v4l2_dv_timings_cap *cap)
{
	*cap = hws_timings_cap;
	return 0;
}

static int hws_log_status(struct file *file, void *fh)
{
	struct hws_chan *c = video_drvdata(file);
	struct v4l2_device *v = &c->card->v4l2_dev;

	v4l2_info(v, "input %u (%s): %s%s\n", c->index + 1, video_device_node_name(&c->vdev),
		  c->streaming ? "capturing" : "idle", c->probing ? ", measuring the rate" : "");
	if (c->in.signal)
		v4l2_info(v, "signal: %ux%u%s, card says %u fps, period %llu ns%s\n",
			  c->in.width, c->in.height, c->in.interlaced ? " interlaced" : "",
			  c->in.fps, c->period_ns, c->in.hdcp ? ", HDCP" : "");
	else
		v4l2_info(v, "signal: none\n");
	v4l2_info(v, "timings %ux%u%s, format %p4cc\n", c->width, c->height,
		  c->interlaced ? "i" : "p", &c->pixfmt);
	v4l2_info(v, "frames %llu skipped %llu (no buffer %llu, no sync %llu) missed %llu dma errors %llu restarts %llu\n",
		  c->stat.frames, c->stat.skipped, c->stat.no_buffer, c->stat.no_sync,
		  c->stat.events_missed, c->stat.dma_errors, c->stat.restarts);
	v4l2_info(v, "done events: video %u audio %u, audio packets %u, lines split by the register move %u\n",
		  c->stat.vdone, c->stat.adone, c->audio_packets, c->stat.split_lines);
	return 0;
}

static int hws_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hws_chan *c = container_of(ctrl->handler, struct hws_chan, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		c->bchs[0] = ctrl->val;
		break;
	case V4L2_CID_CONTRAST:
		c->bchs[1] = ctrl->val;
		break;
	case V4L2_CID_HUE:
		c->bchs[2] = ctrl->val;
		break;
	case V4L2_CID_SATURATION:
		c->bchs[3] = ctrl->val;
		break;
	default:
		return -EINVAL;
	}
	hws_apply_bchs(c);
	return 0;
}

static int hws_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hws_chan *c = container_of(ctrl->handler, struct hws_chan, ctrl_handler);
	struct hws_input in;

	if (ctrl->id != V4L2_CID_DV_RX_POWER_PRESENT)
		return -EINVAL;
	hws_read_input(c->card, c->index, &in);
	ctrl->val = in.signal;
	return 0;
}

static const struct v4l2_ctrl_ops hws_ctrl_ops = {
	.s_ctrl = hws_s_ctrl,
	.g_volatile_ctrl = hws_g_volatile_ctrl,
};

static int hws_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops hws_ioctl_ops = {
	.vidioc_querycap = hws_querycap,
	.vidioc_enum_fmt_vid_cap = hws_enum_fmt,
	.vidioc_g_fmt_vid_cap_mplane = hws_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = hws_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = hws_s_fmt,
	.vidioc_enum_input = hws_enum_input,
	.vidioc_g_input = hws_g_input,
	.vidioc_s_input = hws_s_input,
	.vidioc_query_dv_timings = hws_query_dv_timings,
	.vidioc_s_dv_timings = hws_s_dv_timings,
	.vidioc_g_dv_timings = hws_g_dv_timings,
	.vidioc_enum_dv_timings = hws_enum_dv_timings,
	.vidioc_dv_timings_cap = hws_dv_timings_cap,
	.vidioc_log_status = hws_log_status,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = hws_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations hws_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

/* ---- memory ---------------------------------------------------------- */

static bool hws_same_page(dma_addr_t a, dma_addr_t b)
{
	return upper_32_bits(a) == upper_32_bits(b) &&
	       (lower_32_bits(a) & HWS_PAGE_MASK) == (lower_32_bits(b) & HWS_PAGE_MASK);
}

static bool hws_fits_page(dma_addr_t dma, size_t bytes)
{
	return hws_same_page(dma, dma + bytes - 1);
}

/*
 * A coherent buffer inside the 512 MiB page of the channel (the page of the
 * first buffer allocated, or any page for the first). The allocator is
 * asked again while it hands out memory elsewhere; what it gave meanwhile
 * is held so that it does not give the same again, and freed at the end.
 */
static void *hws_alloc_in_page(struct hws_chan *c, size_t bytes, dma_addr_t *dma, bool first)
{
	struct device *dev = &c->card->pdev->dev;
	struct { void *cpu; dma_addr_t dma; } rejected[8];
	unsigned int nrej = 0, i;
	void *cpu = NULL;

	while (nrej < ARRAY_SIZE(rejected)) {
		cpu = dma_alloc_coherent(dev, bytes, dma, GFP_KERNEL);
		if (!cpu)
			break;
		if (hws_fits_page(*dma, bytes) && (first || hws_same_page(*dma, c->slots[0].dma)))
			break;
		rejected[nrej].cpu = cpu;
		rejected[nrej].dma = *dma;
		nrej++;
		cpu = NULL;
	}
	for (i = 0; i < nrej; i++)
		dma_free_coherent(dev, bytes, rejected[i].cpu, rejected[i].dma);
	return cpu;
}

int hws_chan_alloc(struct hws_chan *c)
{
	unsigned int i;

	for (i = 0; i < HWS_NUM_SLOTS; i++) {
		c->slots[i].cpu = hws_alloc_in_page(c, HWS_SLOT_BYTES, &c->slots[i].dma, i == 0);
		if (!c->slots[i].cpu)
			goto err;
	}
	if (c->has_audio) {
		c->aud_cpu = hws_alloc_in_page(c, 2 * HWS_AUDIO_PACKET_BYTES, &c->aud_dma, false);
		c->aring = vzalloc(HWS_AUDIO_RING_FRAMES * sizeof(u32));
		if (!c->aud_cpu || !c->aring)
			goto err;
	}
	return 0;
err:
	dev_err(&c->card->pdev->dev, "input %u: no DMA memory within one 512 MiB page\n",
		c->index + 1);
	hws_chan_free(c);
	return -ENOMEM;
}

void hws_chan_free(struct hws_chan *c)
{
	struct device *dev = &c->card->pdev->dev;
	unsigned int i;

	for (i = 0; i < HWS_NUM_SLOTS; i++) {
		if (c->slots[i].cpu)
			dma_free_coherent(dev, HWS_SLOT_BYTES, c->slots[i].cpu, c->slots[i].dma);
		c->slots[i].cpu = NULL;
	}
	if (c->aud_cpu)
		dma_free_coherent(dev, 2 * HWS_AUDIO_PACKET_BYTES, c->aud_cpu, c->aud_dma);
	c->aud_cpu = NULL;
	vfree(c->aring);
	c->aring = NULL;
}

/* ---- registration ---------------------------------------------------- */

int hws_video_register(struct hws_chan *c)
{
	struct vb2_queue *q = &c->queue;
	struct video_device *vdev = &c->vdev;
	struct hws_card *card = c->card;
	struct v4l2_ctrl *ctrl;
	int ret;

	mutex_init(&c->lock);
	INIT_LIST_HEAD(&c->queued);
	INIT_LIST_HEAD(&c->waiting);
	INIT_WORK(&c->done_work, hws_done_work);
	INIT_DELAYED_WORK(&c->watchdog, hws_watchdog);
	c->pixfmt = SDI_PIX_FMT_UYVY;
	c->probe_until = jiffies;
	hws_timings_for(1920, 1080, false, NSEC_PER_SEC / 60, &c->timings);
	hws_update_geometry(c);
	hws_read_input(card, c->index, &c->in);

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = c;
	q->buf_struct_size = sizeof(struct hws_buffer);
	q->ops = &hws_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	hws_queue_min_buffers(q, 2);
	q->lock = &c->lock;
	q->dev = &card->pdev->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(&c->ctrl_handler, 5);
	v4l2_ctrl_new_std(&c->ctrl_handler, &hws_ctrl_ops, V4L2_CID_BRIGHTNESS, 0, 255, 1, 0x80);
	v4l2_ctrl_new_std(&c->ctrl_handler, &hws_ctrl_ops, V4L2_CID_CONTRAST, 0, 255, 1, 0x80);
	v4l2_ctrl_new_std(&c->ctrl_handler, &hws_ctrl_ops, V4L2_CID_SATURATION, 0, 255, 1, 0x80);
	v4l2_ctrl_new_std(&c->ctrl_handler, &hws_ctrl_ops, V4L2_CID_HUE, 0, 255, 1, 0);
	ctrl = v4l2_ctrl_new_std(&c->ctrl_handler, &hws_ctrl_ops, V4L2_CID_DV_RX_POWER_PRESENT,
				 0, 1, 0, 0);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY;
	ret = c->ctrl_handler.error;
	if (ret)
		goto err_ctrl;
	v4l2_ctrl_handler_setup(&c->ctrl_handler);

	snprintf(vdev->name, sizeof(vdev->name), "%s %s in %u", card->short_name, hws_kind(c),
		 c->index + 1);
	vdev->fops = &hws_fops;
	vdev->ioctl_ops = &hws_ioctl_ops;
	/* The card goes with its last node (hws_card_release()); the node itself is part of it. */
	vdev->release = video_device_release_empty;
	vdev->dev.groups = hws_node_groups;
	vdev->lock = &c->lock;
	vdev->queue = q;
	vdev->v4l2_dev = &card->v4l2_dev;
	vdev->ctrl_handler = &c->ctrl_handler;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, c);

	/* Media graph: connector "HDMI n" -> the node. */
	c->vdev_pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, &c->vdev_pad);
	if (ret)
		goto err_ctrl;
	snprintf(c->connector_name, sizeof(c->connector_name), "%s %u", hws_kind(c), c->index + 1);
	c->connector.name = c->connector_name;
	c->connector.function = MEDIA_ENT_F_DV_DECODER;
	c->connector_pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&c->connector, 1, &c->connector_pad);
	if (!ret)
		ret = media_device_register_entity(&card->mdev, &c->connector);
	if (ret)
		goto err_entity;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_connector;
	ret = media_create_pad_link(&c->connector, 0, &vdev->entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		dev_warn(&card->pdev->dev, "input %u: no media link (%d)\n", c->index + 1, ret);
	dev_info(&card->pdev->dev, "input %u: %s, %s\n", c->index + 1,
		 video_device_node_name(vdev), c->in.signal ? "signal" : "no signal");
	return 0;
err_connector:
	media_device_unregister_entity(&c->connector);
	media_entity_cleanup(&c->connector);
err_entity:
	media_entity_cleanup(&vdev->entity);
err_ctrl:
	v4l2_ctrl_handler_free(&c->ctrl_handler);
	vb2_queue_release(q);
	return ret;
}

void hws_video_unregister(struct hws_chan *c)
{
	if (!video_is_registered(&c->vdev))
		return;
	mutex_lock(&c->lock);
	if (c->probing) {
		hws_vcap_stop(c);
		c->probing = false;
	}
	mutex_unlock(&c->lock);
	vb2_video_unregister_device(&c->vdev);
	media_device_unregister_entity(&c->connector);
	media_entity_cleanup(&c->connector);
	/* The controls stay until the last file handle, which may hold events of them, is closed. */
}
