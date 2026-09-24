/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Registers of the HWS capture core, BAR0. Everything the driver touches
 * is here; the layout comes from the vendor's driver and the baseline
 * driver of the HWS family (github.com/benhoff/hws), the meaning of each
 * word from what the card was seen to do.
 */
#ifndef HWSV4L2_REGS_H
#define HWSV4L2_REGS_H

#include <linux/bits.h>

/*
 * PCIe bridge: interrupt routing and enables. Of the enables only bits 8,
 * 16 and 17 exist; any of them makes the core's done bits pulses.
 */
#define HWS_REG_INT_EN			0x0134
#define HWS_REG_INT_DEC			0x0138	/* source -> vector map, 0 = all to vector 0 */
#define HWS_REG_BRIDGE_EN		0x0148	/* 1 = bridge on */

/*
 * Address remap table: one 64-bit entry per channel, high word then the
 * 512 MiB page of the low word. The capture core addresses channel ch as
 * (ch + 1) * HWS_AXI_WINDOW + offset into that page, video and audio alike,
 * so everything a channel writes lives in one 512 MiB page of host memory.
 */
#define HWS_REG_REMAP(ch)		(0x0208 + (ch) * 8)
#define HWS_AXI_WINDOW			0x20000000u
#define HWS_PAGE_MASK			0xe0000000u
#define HWS_PAGE_OFFSET			0x1fffffffu

/* The capture core, a bank of 32-bit words from 0x4000. */
#define HWS_CORE			0x4000
#define HWS_CORE_REG(n)			(HWS_CORE + (n) * 4)

/*
 * Word 0 reads the system status (bit 0 running, bit 3 DMA busy) and is
 * written as the decoder mode: 0 resets the core, bit 31 starts it, the
 * low byte picks the output format (0x13: YUYV, BT.709), 0x10 stops it.
 */
#define HWS_REG_STATUS			HWS_CORE_REG(0)
#define HWS_REG_DEC_MODE		HWS_CORE_REG(0)
#define HWS_STATUS_RUNNING		BIT(0)
#define HWS_STATUS_DMA_BUSY		BIT(3)
#define HWS_DEC_MODE_STOP		0x10
#define HWS_DEC_MODE_START		0x80000000u
#define HWS_DEC_MODE_START_ALL		0x80ffffffu
#define HWS_DEC_MODE_YUYV		0x13

/* Done events, write 1 to clear: video bits 0-3, audio 8-11. */
#define HWS_REG_INT_STATUS		HWS_CORE_REG(1)
#define HWS_INT_VDONE(ch)		BIT(ch)
#define HWS_INT_ADONE(ch)		BIT(8 + (ch))

#define HWS_REG_VCAP_ENABLE		HWS_CORE_REG(2)	/* bit per channel */
#define HWS_REG_ACAP_ENABLE		HWS_CORE_REG(3)	/* bit per channel */
#define HWS_REG_CTL			HWS_CORE_REG(4)
#define HWS_CTL_IRQ_ENABLE		BIT(0)
/* Bits 0-3: the input carries a signal; bits 8-11: it is interlaced. */
#define HWS_REG_ACTIVE			HWS_CORE_REG(5)
#define HWS_ACTIVE_SIGNAL(ch)		BIT(ch)
#define HWS_ACTIVE_INTERLACED(ch)	BIT(8 + (ch))
#define HWS_REG_HDCP			HWS_CORE_REG(8)	/* bit per channel: HDCP on the input */
#define HWS_REG_DMA_MAX			HWS_CORE_REG(9)	/* largest transfer, 16-byte units */

/* Per channel: where the frame goes (window address) and half its size /16. */
#define HWS_REG_VBUF(ch)		HWS_CORE_REG(16 + (ch))
#define HWS_REG_AUDBUF(ch)		HWS_CORE_REG(24 + (ch))
#define HWS_REG_VHALF(ch)		HWS_CORE_REG(50 + (ch))
/* Which half of its two-packet ring the audio engine is writing now. */
#define HWS_REG_ABUF_TOGGLE(ch)		HWS_CORE_REG(40 + (ch))
#define HWS_REG_VBUF_TOGGLE(ch)		HWS_CORE_REG(32 + (ch))

/* Board version: bits 7..0 version, 15..8 sub-version. */
#define HWS_REG_DEVICE_INFO		HWS_CORE_REG(88)

/* Input raster as measured: width in bits 15..0, height 31..16. */
#define HWS_REG_IN_RES(ch)		HWS_CORE_REG(90 + (ch) * 2)
/* Brightness, contrast, hue, saturation: one byte each from bit 0. */
#define HWS_REG_BCHS(ch)		HWS_CORE_REG(91 + (ch) * 2)
#define HWS_REG_IN_FPS(ch)		HWS_CORE_REG(110 + (ch))	/* whole frames a second */
#define HWS_REG_OUT_RES(ch)		HWS_CORE_REG(120 + (ch))	/* scaler output, like IN_RES */

/* The largest frame the scaler delivers. */
#define HWS_MAX_WIDTH			1920
#define HWS_MAX_HEIGHT			1080

/* One audio packet: 1024 stereo frames of 16-bit samples, 48 kHz. */
#define HWS_AUDIO_PACKET_FRAMES		1024
#define HWS_AUDIO_PACKET_BYTES		(HWS_AUDIO_PACKET_FRAMES * 4)

#endif
