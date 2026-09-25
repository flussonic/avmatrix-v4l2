/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HWSV4L2_H
#define HWSV4L2_H

#include <linux/version.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ctrls.h>
#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "hwsv4l2_regs.h"
#include "../include/hwav.h"

#define HWS_DRV_NAME		"hwsv4l2"
#ifndef HWS_VERSION
#define HWS_VERSION		"0"
#endif
#define HWS_MAX_CHANNELS	4

/* What older kernels call differently; the meaning is the same. */
#ifndef PCI_IRQ_INTX
#define PCI_IRQ_INTX		PCI_IRQ_LEGACY	/* renamed in 6.8 */
#endif

/* Until 6.13 vb2 dropped the queue lock around waits through these two
 * callbacks; since then it takes q->lock by itself. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
#define HWS_VB2_WAIT_OPS	.wait_prepare = vb2_ops_wait_prepare, \
				.wait_finish = vb2_ops_wait_finish,
#else
#define HWS_VB2_WAIT_OPS
#endif

/* vb2 renamed the field in 6.8; what it means did not change. */
static inline void hws_queue_min_buffers(struct vb2_queue *q, unsigned int n)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
	q->min_buffers_needed = n;
#else
	q->min_queued_buffers = n;
#endif
}

/*
 * Frame slots of a channel in host memory. The engine writes each line of a
 * frame to the buffer register's address plus the line's offset, reading
 * the register as it goes; the driver moves the register to a fresh slot in
 * the middle of every frame, so a frame lies in two slots -- its top in the
 * slot it started in, its bottom in the next -- split at a line found from
 * markers the driver writes at the start of every line of a slot it hands
 * out. A slot is held by the register and by every frame with a part in it.
 */
#define HWS_NUM_SLOTS		4
#define HWS_SLOT_BYTES		PAGE_ALIGN(HWS_MAX_WIDTH * 2 * HWS_MAX_HEIGHT)

struct hws_slot {
	void *cpu;
	dma_addr_t dma;
	unsigned int users;	/* the register, and frames with a part in it */
	u32 mark_at;		/* the line the markers were laid out around */
	u32 band_lo, band_hi;	/* lines marked whole around it */
	bool ready;		/* marked, and not handed out since */
	bool marking;		/* being marked outside the poll */
};

/* A frame as it lies in the slots. */
struct hws_frame {
	struct hws_slot *top;		/* lines 0 .. cut - 1 */
	struct hws_slot *bottom;	/* lines cut .. height - 1 */
	u32 cut;
	u32 sequence;
	u64 ts;
	u32 split_word;			/* line cut - 1 crosses at this word; 0: it does not */
	bool split_lost;		/* it crosses outside the marked band: where is not known */
	bool copying;			/* the work item is copying it */
};

/* Completed frames waiting for their copy. */
#define HWS_NUM_FRAMES		4

/* Software ring of received audio: stereo frames, one u32 each (L low, R high). */
#define HWS_AUDIO_RING_FRAMES	65536
/* Done events of audio packets kept to place a video frame in the audio. */
#define HWS_AUDIO_MARKS		64
/* A buffer waits at most this long past its frame for the audio to catch up. */
#define HWS_AUDIO_WAIT_NS	(100ULL * NSEC_PER_MSEC)

struct hws_audio_mark {
	u64 ts;			/* the packet's done event */
	u64 end;		/* frames received once it was in */
};

struct hws_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	u64 ts;			/* the frame's done event */
	u64 period_ns;		/* frame period when it was captured */
	bool bad;		/* the frame did not arrive whole */
};

/* What the card says about the input of a channel, read by the monitor. */
struct hws_input {
	bool signal;
	bool interlaced;
	bool hdcp;
	u32 width;		/* raster as measured */
	u32 height;
	u32 fps;		/* the card's whole-number count, 0 if none */
};

struct hws_card;

struct hws_chan {
	struct hws_card *card;
	unsigned int index;

	struct video_device vdev;
	struct media_pad vdev_pad;
	struct media_entity connector;
	struct media_pad connector_pad;
	char connector_name[16];
	struct vb2_queue queue;
	struct v4l2_ctrl_handler ctrl_handler;
	struct mutex lock;		/* serialises ioctls and the queue */
	spinlock_t event_lock;		/* slots, lists, audio ring, measurement */

	struct hws_slot slots[HWS_NUM_SLOTS];
	struct hws_slot *base;		/* the slot in the buffer register */
	struct hws_frame cur;		/* the frame the engine is writing */
	struct hws_frame frames[HWS_NUM_FRAMES];	/* complete, waiting for the copy */
	unsigned int nframes;
	void *aud_cpu;			/* the card's two-packet ring */
	dma_addr_t aud_dma;
	bool has_audio;

	/* The input and what the driver made of it. */
	struct hws_input in;
	u64 period_ns;			/* measured frame period, 0 until known */
	bool probing;			/* capture runs only to measure the rate */
	unsigned long probe_until;	/* jiffies */
	u64 m_first_ns;			/* measurement window */
	u64 m_last_ns;
	u32 m_frames;

	/* Capture configuration: the frame the client asked for. */
	struct v4l2_dv_timings timings;
	u32 width, height;		/* the frame; interlaced frames are woven */
	bool interlaced;
	u32 pixfmt;			/* SDI_PIX_FMT_UYVY or _YUYV */
	u32 row_bytes;			/* bytes of a line in the plane */
	u32 card_row_bytes;		/* bytes of a line as the card writes it */
	u32 frame_bytes;		/* card bytes of a frame */

	/* Capture state. */
	bool streaming;
	bool vcap_on;
	bool arm_pending;		/* the register moves to a fresh slot at arm_at */
	u64 arm_at;
	u32 sequence;			/* done events since STREAMON, lost ones included */
	u64 last_done_ns;
	struct list_head queued;	/* buffers the client gave, empty */
	struct list_head waiting;	/* buffers with a picture, waiting for their audio */
	struct work_struct done_work;
	struct delayed_work watchdog;

	/* Audio. */
	u32 *aring;
	u64 a_total;			/* stereo frames received since STREAMON */
	struct hws_audio_mark marks[HWS_AUDIO_MARKS];
	unsigned int nmarks;		/* valid entries, newest at marks[nmarks - 1] */
	u64 a_frame_end;		/* ring position where the last delivered frame's audio ended */
	bool a_started;
	u32 audio_packets;

	/* Colour controls, applied to the card as set. */
	u8 bchs[4];

	struct {
		u64 frames;		/* buffers handed to user space */
		u64 skipped;		/* frames the card had and the client did not get */
		u64 no_buffer;		/* of those: nothing queued */
		u64 no_sync;		/* frames of another raster or rate than the one set */
		u64 resyncs;		/* frames given up because the register moved late */
		u64 events_missed;	/* done events not seen, by their spacing */
		u64 dma_errors;		/* frames that did not arrive whole */
		u64 restarts;		/* capture restarts by the watchdog */
		u32 vdone, adone, split_lines;
	} stat;
};

struct hws_card {
	struct pci_dev *pdev;
	void __iomem *bar0;
	const char *name;		/* the product */
	const char *short_name;		/* the product without its interface */
	bool sdi;			/* SDI inputs rather than HDMI */
	unsigned int nch;		/* video inputs */
	unsigned int naudio;		/* of those with audio */
	u32 device_ver;
	u32 sub_ver;
	spinlock_t reg_lock;		/* read-modify-write of shared registers */
	struct mutex start_lock;	/* starting the core again while channels run */
	struct hws_chan *ch[HWS_MAX_CHANNELS];
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct delayed_work monitor;
	/*
	 * Where the channels copy their frames: unbound, so that two channels
	 * copy on two CPUs at once rather than one after the other on the CPU
	 * the poll ran on -- each copy has half a frame before the next frame
	 * reaches it.
	 */
	struct workqueue_struct *wq;
	struct hrtimer poll_timer;
	struct mutex poll_lock;
	unsigned int poll_users;	/* engines running; the poll runs while any does */
	u64 last_poll_ns;
	bool running;			/* the core is started */
};

static inline u32 hws_rd(struct hws_card *card, u32 reg)
{
	return ioread32(card->bar0 + reg);
}

static inline void hws_wr(struct hws_card *card, u32 reg, u32 val)
{
	iowrite32(val, card->bar0 + reg);
}

/* The status poll, hwsv4l2_module.c. */
void hws_poll_get(struct hws_card *card);
void hws_poll_put(struct hws_card *card);

/* Card, hwsv4l2_hw.c. */
int hws_card_start(struct hws_card *card);
void hws_card_stop(struct hws_card *card);
int hws_card_check(struct hws_card *card);
void hws_restore_channels(struct hws_card *card);
void hws_set_bits(struct hws_card *card, u32 reg, u32 bits, bool on);
void hws_read_input(struct hws_card *card, unsigned int ch, struct hws_input *in);
void hws_program_window(struct hws_chan *c, dma_addr_t dma, u32 reg);
void hws_apply_bchs(struct hws_chan *c);

/* Timings, hwsv4l2_timings.c. */
extern const struct v4l2_dv_timings_cap hws_timings_cap;
void hws_frame_of_input(const struct hws_input *in, u32 *w, u32 *h, bool *interlaced);
u64 hws_snap_period(u64 measured_ns, bool *known);
void hws_timings_for(u32 width, u32 height, bool interlaced, u64 period_ns,
		     struct v4l2_dv_timings *t);
u64 hws_timings_period(const struct v4l2_dv_timings *t);
bool hws_timings_fit(const struct v4l2_dv_timings *t);

/* Capture node, hwsv4l2_video.c. */
int hws_video_register(struct hws_chan *c);
void hws_video_unregister(struct hws_chan *c);
void hws_video_done(struct hws_chan *c, u64 now_ns);
void hws_video_poll(struct hws_chan *c, u64 now_ns);
void hws_video_input_changed(struct hws_chan *c);
void hws_video_probe_tick(struct hws_chan *c);
int hws_chan_alloc(struct hws_chan *c);
void hws_chan_free(struct hws_chan *c);

/* Audio, hwsv4l2_audio.c. */
void hws_audio_done(struct hws_chan *c, u64 now_ns);
void hws_audio_start(struct hws_chan *c);
void hws_audio_stop(struct hws_chan *c);
bool hws_audio_ready(struct hws_chan *c, u64 ts, u64 now_ns);
unsigned int hws_audio_take(struct hws_chan *c, u64 ts, void *plane, size_t bytes);

/* Counters in sysfs, hwsv4l2_sysfs.c: the attribute groups of a node. */
extern const struct attribute_group *hws_node_groups[];

#endif
