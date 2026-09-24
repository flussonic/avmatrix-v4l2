// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stream counters in sysfs, next to each video node
 * (/sys/class/video4linux/videoN/): reset at STREAMON, readable by anyone,
 * meant for monitoring. The names and their meanings are the contract in
 * docs/sdi-sysfs.md, shared with the other cards: a counter the hardware
 * does not report has no file here -- this card counts no line CRCs -- so
 * that a reader tells "nothing to report" from a genuine zero.
 * VIDIOC_LOG_STATUS prints the same picture to the kernel log; the card's
 * identity is in the media device (MEDIA_IOC_DEVICE_INFO).
 */
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/math64.h>

#include "hwsv4l2.h"

static struct hws_chan *chan_of(struct device *dev)
{
	struct video_device *vdev = container_of(dev, struct video_device, dev);

	return video_get_drvdata(vdev);
}

#define HWS_COUNTER(name, field)						\
static ssize_t name##_show(struct device *dev, struct device_attribute *attr,	\
			   char *buf)						\
{										\
	return sysfs_emit(buf, "%llu\n", chan_of(dev)->stat.field);		\
}										\
static DEVICE_ATTR_RO(name)

HWS_COUNTER(frames, frames);			/* buffers handed to user space */
HWS_COUNTER(frames_skipped, skipped);		/* frames of the card the client did not get */
HWS_COUNTER(no_buffer, no_buffer);		/* of those: nothing queued by the client */
HWS_COUNTER(no_sync, no_sync);			/* frames of another raster or rate */
HWS_COUNTER(resyncs, resyncs);			/* frames given up to a slot handed out late */
HWS_COUNTER(events_missed, events_missed);	/* done events lost, by their spacing */
HWS_COUNTER(dma_errors, dma_errors);		/* frames that did not arrive whole */
HWS_COUNTER(restarts, restarts);		/* capture restarts by the watchdog */

static ssize_t signal_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hws_chan *c = chan_of(dev);
	struct hws_input in;
	u64 period = c->period_ns;

	hws_read_input(c->card, c->index, &in);
	if (!in.signal)
		return sysfs_emit(buf, "no signal\n");
	if (!period)
		return sysfs_emit(buf, "%ux%u%s, measuring the rate%s\n", in.width, in.height,
				  in.interlaced ? " interlaced" : "", in.hdcp ? ", HDCP" : "");
	return sysfs_emit(buf, "%ux%u%s %llu.%03llu fps%s%s\n", in.width, in.height,
			  in.interlaced ? " interlaced" : "",
			  div64_u64(NSEC_PER_SEC, period),
			  div64_u64(NSEC_PER_SEC * 1000ULL, period) % 1000,
			  in.hdcp ? ", HDCP" : "", c->streaming ? " capturing" : "");
}
static DEVICE_ATTR_RO(signal);

/* Of this driver: whether the source protects the input with HDCP. */
static ssize_t hdcp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hws_chan *c = chan_of(dev);
	struct hws_input in;

	hws_read_input(c->card, c->index, &in);
	return sysfs_emit(buf, "%d\n", in.signal && in.hdcp);
}
static DEVICE_ATTR_RO(hdcp);

static struct attribute *hws_node_attrs[] = {
	&dev_attr_frames.attr,
	&dev_attr_frames_skipped.attr,
	&dev_attr_no_buffer.attr,
	&dev_attr_no_sync.attr,
	&dev_attr_resyncs.attr,
	&dev_attr_events_missed.attr,
	&dev_attr_dma_errors.attr,
	&dev_attr_restarts.attr,
	&dev_attr_signal.attr,
	&dev_attr_hdcp.attr,
	NULL,
};

static const struct attribute_group hws_node_group = {
	.attrs = hws_node_attrs,
};

int hws_sysfs_add(struct hws_chan *c)
{
	return sysfs_create_group(&c->vdev.dev.kobj, &hws_node_group);
}

void hws_sysfs_remove(struct hws_chan *c)
{
	sysfs_remove_group(&c->vdev.dev.kobj, &hws_node_group);
}
