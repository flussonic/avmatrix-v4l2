/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * What an AVMatrix HWS card adds to the frame contract of sdi_av.h: the
 * vendor block of the metadata plane. The common part of the plane names it
 * by vendor_magic HWAV and vendor_version; a client that knows neither reads
 * the common part and stops there.
 */
#ifndef HWAV_H
#define HWAV_H

#include "sdi_av.h"

#define HWAV_VENDOR_MAGIC	v4l2_fourcc('H', 'W', 'A', 'V')	/* an AVMatrix HWS card */
#define HWAV_VENDOR_VERSION	1

#define HWAV_F_HDCP		(1u << 0) /* the input is HDCP protected; the card delivers no picture */
#define HWAV_F_SCALED		(1u << 1) /* the input raster is not the frame's: the card scaled it */

/* Vendor block of an HWS card: right after the common part. */
struct hwav_meta {
	__u32 flags;		/* HWAV_F_* */
	__u32 in_width;		/* raster of the input as the card measured it */
	__u32 in_height;	/* lines of a frame, or of a field when interlaced */
	__u32 in_fps;		/* whole frames a second as the card counts them, 0 if it does not */
	__u32 frame_period_ns;	/* frame period the driver measured */
	__u32 audio_packets;	/* audio packets the card delivered since STREAMON */
	__u32 reserved[6];
};

/* Bytes of the metadata plane a capture buffer carries: sdi_meta then the block. */
#define HWAV_META_BYTES		(SDI_META_SIZE + sizeof(struct hwav_meta))

#endif
