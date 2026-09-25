// SPDX-License-Identifier: GPL-2.0-only
/*
 * From what the card measures to DV timings.
 *
 * The card reports the raster of the input and a whole number of frames a
 * second, which cannot tell 30 from 29.97; the frame period comes from the
 * driver's own clock instead, the spacing of the done events over two
 * seconds, snapped to the rate it is closest to. The timings are then
 * the CEA-861 or DMT preset of that raster and rate when there is one (a
 * 1000/1001 rate as the whole rate's preset with V4L2_DV_FL_REDUCED_FPS, the
 * way v4l2_calc_timeperframe() reads it), and a bare frame otherwise.
 */
#include <linux/math64.h>
#include <media/v4l2-dv-timings.h>

#include "hwsv4l2.h"

/*
 * The frame of an input as the card delivers it: an interlaced input is
 * measured by its field and delivered woven, and a raster larger than the
 * scaler's output comes scaled down to it.
 */
/*
 * What the capture takes: CEA-861 and DMT rasters up to the scaler's output.
 * The pixel clock floor is that of a bare frame (hws_timings_for()) of the
 * smallest raster at the lowest rate, no blanking counted.
 */
const struct v4l2_dv_timings_cap hws_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 640,
		.max_width = HWS_MAX_WIDTH,
		.min_height = 480,
		.max_height = HWS_MAX_HEIGHT,
		.min_pixelclock = 5000000,
		.max_pixelclock = 600000000,
		.standards = V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT,
		.capabilities = V4L2_DV_BT_CAP_INTERLACED | V4L2_DV_BT_CAP_PROGRESSIVE,
	},
};

/*
 * The frame of an input as the card delivers it: an interlaced input is
 * measured by its field and delivered woven, and a raster larger than the
 * scaler's output comes scaled down to it. A raster below what the capture
 * takes, or of an odd width, is no frame (0x0): the card reports such while
 * it is still locking.
 */
void hws_frame_of_input(const struct hws_input *in, u32 *w, u32 *h, bool *interlaced)
{
	*w = in->width;
	*h = in->interlaced ? in->height * 2 : in->height;
	*interlaced = in->interlaced;
	if (*w > HWS_MAX_WIDTH || *h > HWS_MAX_HEIGHT) {
		*w = HWS_MAX_WIDTH;
		*h = HWS_MAX_HEIGHT;
	}
	if (*w < hws_timings_cap.bt.min_width || *h < hws_timings_cap.bt.min_height || (*w & 1)) {
		*w = 0;
		*h = 0;
	}
}

/* Rates a source is likely to send, as frames per 1001 or 1000 seconds. */
static const struct {
	u32 num, den;
} hws_rates[] = {
	{ 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 },
	{ 48, 1 }, { 50, 1 }, { 56, 1 }, { 60000, 1001 }, { 60, 1 }, { 70, 1 },
	{ 72, 1 }, { 75, 1 }, { 85, 1 }, { 100, 1 }, { 120000, 1001 }, { 120, 1 },
};

/*
 * The period of the nearest rate when the measurement is within 0.033% of
 * it -- a third of the distance between 30 and 29.97 -- and the
 * measurement itself otherwise, flagged as not a known rate.
 */
u64 hws_snap_period(u64 measured_ns, bool *known)
{
	u64 best = 0, best_diff = U64_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hws_rates); i++) {
		u64 p = div_u64((u64)hws_rates[i].den * NSEC_PER_SEC, hws_rates[i].num);
		u64 d = p > measured_ns ? p - measured_ns : measured_ns - p;

		if (d < best_diff) {
			best_diff = d;
			best = p;
		}
	}
	*known = best_diff * 3000 <= best;
	return *known ? best : measured_ns;
}

/*
 * The frame totals are summed in 64 bits: the fields come from the client,
 * and their sum in 32 bits can wrap. Totals past 16 bits are no raster.
 */
static u64 hws_bt_period(const struct v4l2_bt_timings *bt)
{
	u64 fw = (u64)bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch;
	u64 fh = (u64)bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch +
		 bt->il_vfrontporch + bt->il_vsync + bt->il_vbackporch;
	u64 p;

	if (!fw || !fh || fw > U16_MAX || fh > U16_MAX || !bt->pixelclock)
		return 0;
	p = div64_u64(fw * fh * NSEC_PER_SEC, bt->pixelclock);
	if (bt->flags & V4L2_DV_FL_REDUCED_FPS)
		p = div_u64(p * 1001, 1000);
	return p;
}

u64 hws_timings_period(const struct v4l2_dv_timings *t)
{
	return t->type == V4L2_DV_BT_656_1120 ? hws_bt_period(&t->bt) : 0;
}

static bool hws_bt_matches(const struct v4l2_bt_timings *bt, u32 w, u32 h, bool interlaced,
			   u64 period)
{
	u64 p = hws_bt_period(bt);

	if (bt->width != w || bt->height != h || !!bt->interlaced != interlaced || !p)
		return false;
	/* Within 0.02%: far closer than 30 is to 29.97. */
	return (p > period ? p - period : period - p) * 5000 <= period;
}

void hws_timings_for(u32 width, u32 height, bool interlaced, u64 period_ns,
		     struct v4l2_dv_timings *t)
{
	const struct v4l2_dv_timings *p;
	unsigned int i;

	for (i = 0; v4l2_dv_timings_presets[i].bt.width; i++) {
		p = &v4l2_dv_timings_presets[i];
		if (hws_bt_matches(&p->bt, width, height, interlaced, period_ns)) {
			*t = *p;
			return;
		}
	}
	for (i = 0; v4l2_dv_timings_presets[i].bt.width; i++) {
		struct v4l2_dv_timings q;

		p = &v4l2_dv_timings_presets[i];
		if (!(p->bt.flags & V4L2_DV_FL_CAN_REDUCE_FPS))
			continue;
		q = *p;
		q.bt.flags |= V4L2_DV_FL_REDUCED_FPS;
		if (hws_bt_matches(&q.bt, width, height, interlaced, period_ns)) {
			*t = q;
			return;
		}
	}
	/* No preset: the active frame alone, clocked to give the period. */
	memset(t, 0, sizeof(*t));
	t->type = V4L2_DV_BT_656_1120;
	t->bt.width = width;
	t->bt.height = height;
	t->bt.interlaced = interlaced;
	if (interlaced)
		t->bt.il_vsync = 1;
	if (period_ns)
		t->bt.pixelclock = div64_u64((u64)width * (height + (interlaced ? 1 : 0)) *
					     NSEC_PER_SEC, period_ns);
	t->bt.standards = 0;
}

/* Timings a client may set: within the cap, an even width (a line is whole words), a period. */
bool hws_timings_fit(const struct v4l2_dv_timings *t)
{
	return v4l2_valid_dv_timings(t, &hws_timings_cap, NULL, NULL) && !(t->bt.width & 1) &&
	       hws_bt_period(&t->bt);
}
