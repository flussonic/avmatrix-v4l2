/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * SDI frame contract of our V4L2 capture and playout drivers: one SDI frame
 * of one port in a single multi-planar V4L2 buffer -- the active picture,
 * the audio of that frame, the ancillary data packets, the vertical
 * blanking lines and per-frame metadata. This file is the same, byte for
 * byte, in every driver that follows the contract and in the client, so a
 * client reads and writes any card with one piece of code. Nothing here
 * belongs to one vendor: the card is named by QUERYCAP (driver, card,
 * bus_info) and the media controller, and by the vendor tail of the
 * metadata when it has something of its own to tell.
 *
 * A capture node is VIDEO_CAPTURE_MPLANE, a playout node VIDEO_OUTPUT_MPLANE,
 * both with the same five planes and the same layout of each; ENUM_FMT says
 * which pixel formats a node offers, G_DV_TIMINGS the geometry of the frame,
 * ENUMINPUT.status and V4L2_EVENT_SOURCE_CHANGE the state of the input.
 * A 1000/1001 rate is the CEA-861 timings of the whole rate with
 * V4L2_DV_FL_REDUCED_FPS and the nominal pixel clock, the way
 * v4l2_calc_timeperframe() reads it; a driver also takes the clock already
 * divided by 1.001, and flags it the same.
 */
#ifndef SDI_AV_H
#define SDI_AV_H

#include <linux/types.h>
#include <linux/videodev2.h>

/* Buffer planes. */
#define SDI_PLANE_VIDEO		0
#define SDI_PLANE_AUDIO		1
#define SDI_PLANE_ANC		2
#define SDI_PLANE_META		3
#define SDI_PLANE_VBI		4
#define SDI_NUM_PLANES		5

/*
 * On output the start of every plane (USERPTR, DMABUF) is aligned to
 * SDI_OUT_PLANE_ALIGN bytes; a card whose DMA engine cannot take another
 * offset answers QBUF with EINVAL. Capture accepts any offset.
 */
#define SDI_OUT_PLANE_ALIGN	64

/*
 * Video plane: the whole active picture; interlaced standards are woven line
 * by line (field 1 line, field 2 line, ...). Dedicated fourccs, so that a
 * foreign client does not mistake the multi-plane buffer for a plain
 * single-plane picture. A node offers a subset; ENUM_FMT names it.
 */
#define SDI_PIX_FMT_UYVY	v4l2_fourcc('S', 'D', 'U', 'Y') /* 4:2:2, 8 bit, Cb Y Cr Y (V4L2_PIX_FMT_UYVY) */
#define SDI_PIX_FMT_YUYV	v4l2_fourcc('S', 'D', 'Y', 'U') /* 4:2:2, 8 bit, Y Cb Y Cr (V4L2_PIX_FMT_YUYV) */
#define SDI_PIX_FMT_YVYU	v4l2_fourcc('S', 'D', 'Y', 'V') /* 4:2:2, 8 bit, Y Cr Y Cb (V4L2_PIX_FMT_YVYU) */
#define SDI_PIX_FMT_UYVY16	v4l2_fourcc('S', 'D', '1', '6') /* 4:2:2, 16-bit words, low 10 bits significant */
#define SDI_PIX_FMT_V210	v4l2_fourcc('S', 'D', '1', '0') /* 4:2:2, 10 bit, three samples per 32-bit word (V4L2_PIX_FMT_V210) */
#define SDI_PIX_FMT_XU10	v4l2_fourcc('S', 'D', 'X', 'U') /* 4:2:2, 10 bit, four samples in the low 40 bits of a 64-bit word */
/*
 * Diagnostics: the lines as the card stored them -- on a card that keeps
 * the whole line (EAV, HANC, SAV, active part) every SDI line of the frame
 * at bytesperline stride, height = total lines; the audio, ANC and VBI
 * planes are still parsed. Not accepted on output.
 */
#define SDI_PIX_FMT_RAW		v4l2_fourcc('S', 'D', 'R', 'W')

/*
 * Audio plane: SDI_AUDIO_CHANNELS channels interleaved, 48 kHz, one sample a
 * 32-bit AES3 subframe (SMPTE 299M / 272M):
 *   bits 31..8  -- 24-bit two's complement sample, MSB aligned;
 *   bit 7       -- Z, channel status block start;
 *   bit 6 -- V, bit 5 -- U, bit 4 -- C, bit 3 -- P;
 *   bits 2..0   -- zero.
 * The samples are passed as received, so a SMPTE 337M data stream (Dolby E,
 * AC-3, ...) arrives intact; audio_nonpcm in the metadata names the channels
 * carrying one, found by the burst preamble or the channel status. A card
 * that does not deliver the status bits leaves bits 7..0 zero; an SD input
 * gives 20 bits (low four bits of the sample zero) unless the embedder sends
 * the 272M extended data packets and the card parses them.
 * All 16 channels are always present in the plane; channels missing from
 * the input carry zero words and audio_present in the metadata says which
 * ones were seen. Field 1 samples come first, then field 2; the boundary is
 * audio_samples in the metadata (a card that knows no field boundary puts
 * everything in [0]). bytesused = samples per channel * SDI_AUDIO_FRAME_BYTES.
 *
 * Output: the client puts the same layout; the card embeds the sample and
 * the U/C bits, Z and P it sets itself. The audio of the frame is what
 * audio_samples in the metadata counts, field by field: samples in
 * bytesused beyond that count are padding and are not played. Without
 * metadata, with audio_samples of zero, or counting more than bytesused
 * holds, the driver takes bytesused as the audio and halves it between the
 * fields. A field holds 48000 / field rate samples, rounded up on the
 * 1000/1001 rates: what does not fit is dropped.
 */
#define SDI_AUDIO_CHANNELS	16
#define SDI_AUDIO_SAMPLE_BYTES	4
#define SDI_AUDIO_FRAME_BYTES	(SDI_AUDIO_CHANNELS * SDI_AUDIO_SAMPLE_BYTES)
#define SDI_AUDIO_RATE		48000
#define SDI_AUDIO_Z		(1u << 7)
#define SDI_AUDIO_V		(1u << 6)
#define SDI_AUDIO_U		(1u << 5)
#define SDI_AUDIO_C		(1u << 4)
#define SDI_AUDIO_P		(1u << 3)
#define SDI_AUDIO_PLANE_SIZE	(256 * 1024)

/*
 * ANC plane: parsed packets back to back, each an sdi_anc_packet header
 * followed by data_count 16-bit UDW words (10 significant bits, parity bits
 * as on the wire), padded to a multiple of 4 bytes. Audio packets (SMPTE
 * 299M/272M) do not appear here -- they are decoded into plane 1; everything
 * else from VANC does, and from HANC on a card that delivers it. The SMPTE
 * 352 payload identifier is here as a packet (DID 0x41 SDID 0x01). bytesused
 * is the total of headers and words.
 *
 * The analogue services of SD blanking (teletext, WSS, VITC, line 21
 * captions) are not packets and are not here: their lines travel as
 * waveforms in plane 4 and the client slices them.
 *
 * Output: the same plane, the same records -- the driver puts them into the
 * blanking lines (SD: words in the multiplex, HD: the Y or C stream by the
 * CHROMA flag); hoffset is not honoured, packets go back to back after what
 * the driver puts there itself (audio, its control packets and, unless the
 * plane carries one, the payload identifier). The list ends at bytesused or
 * at an all-zero header, so a zeroed plane inserts nothing.
 */
#define SDI_ANC_PLANE_SIZE	(256 * 1024)
#define SDI_ANC_MAX_UDW		255

#define SDI_ANC_F_HANC		(1u << 0) /* packet from horizontal blanking, else VANC */
#define SDI_ANC_F_CHROMA	(1u << 1) /* C data stream (HD), else Y; always 0 in SD */
#define SDI_ANC_F_CS_ERROR	(1u << 2) /* packet checksum mismatch */

struct sdi_anc_packet {
	__u16 line;		/* SDI line of the frame (1..total_lines) */
	__u16 hoffset;		/* ADF offset in samples from SAV */
	__u8  did;		/* low 8 bits of DID */
	__u8  sdid;		/* low 8 bits of SDID / DBN */
	__u8  data_count;	/* number of UDW words */
	__u8  flags;		/* SDI_ANC_F_* */
	__u16 udw[];		/* data_count words, then padding */
};

#define SDI_ANC_PACKET_BYTES(dc) \
	((sizeof(struct sdi_anc_packet) + (dc) * 2u + 3u) & ~3u)

/*
 * VBI plane: the luma of the vertical blanking lines of standard definition
 * video as it is on the wire, so that whatever those lines carry -- teletext
 * (ITU-R BT.653), closed captions on line 21, WSS, VITC -- can be sliced by
 * the client; the driver does not interpret them. One row per line,
 * SDI_VBI_SAMPLES 16-bit words with the 10-bit sample in the low bits, the
 * lines of the standard in ascending order: 625-line video rows 0..16 are
 * lines 6..22 and rows 17..33 lines 319..335; 525-line video rows 0..11 are
 * lines 10..21 and rows 12..23 lines 273..284 (sdi_vbi_line() maps a row to
 * its line). bytesused = rows * SDI_VBI_LINE_BYTES; high definition video
 * has no such lines and bytesused is 0.
 *
 * Output: the same plane the other way -- a row within bytesused whose words
 * are not all zero replaces the luma of its line (the chroma stays neutral,
 * and no ancillary packet is put on that line); a zero row, a row past
 * bytesused or a plane with bytesused 0 leaves the line at black.
 */
#define SDI_VBI_SAMPLES		720
#define SDI_VBI_LINE_BYTES	(SDI_VBI_SAMPLES * 2)
#define SDI_VBI_MAX_LINES	34
#define SDI_VBI_PLANE_SIZE	(SDI_VBI_MAX_LINES * SDI_VBI_LINE_BYTES)

/* Rows of the VBI plane for a frame of total_lines lines. */
static inline __u32 sdi_vbi_rows(__u32 total_lines)
{
	return total_lines == 625 ? 34 : total_lines == 525 ? 24 : 0;
}

/* VBI plane row of an SDI line, -1 for a line the plane does not carry. */
static inline int sdi_vbi_row(__u32 total_lines, __u32 line)
{
	if (total_lines == 625) {
		if (line >= 6 && line <= 22)
			return line - 6;
		if (line >= 319 && line <= 335)
			return 17 + line - 319;
	} else if (total_lines == 525) {
		if (line >= 10 && line <= 21)
			return line - 10;
		if (line >= 273 && line <= 284)
			return 12 + line - 273;
	}
	return -1;
}

/* SDI line of a VBI plane row, 0 for a row the standard does not have. */
static inline __u32 sdi_vbi_line(__u32 total_lines, __u32 row)
{
	if (total_lines == 625)
		return row < 17 ? 6 + row : row < 34 ? 319 + row - 17 : 0;
	if (total_lines == 525)
		return row < 12 ? 10 + row : row < 24 ? 273 + row - 12 : 0;
	return 0;
}

/*
 * Metadata plane: one struct sdi_meta per frame. The plane opens with the
 * magic, so a client can tell the buffer follows this contract without
 * asking QUERYCAP, and the version of the layout. The common part is
 * exactly SDI_META_SIZE bytes; fields are only ever added into reserved[],
 * any other change is a new version. Its last three words are the vendor
 * tail: vendor_magic names the card, a fourcc of the driver's choosing (0
 * when the card does not name itself); vendor_bytes is the length of the
 * vendor block that follows the common part (0: no block, and the plane is
 * exactly SDI_META_SIZE bytes); vendor_version the layout version of that
 * block. The vendor block is how a card tells what the common part has no
 * place for: a structure of the driver's own, declared in the driver, not
 * here, with fields only ever appended; a client reads it only knowing the
 * (vendor_magic, vendor_version) pair, and no further than it knows. A
 * client that does not know the pair reads the common part and skips the
 * block. Version 4 gave the metadata this common layout with one magic for
 * every card and the vendor tail; version 3 added the VBI plane and took
 * out of the metadata what V4L2 already says about the buffer:
 *
 *   v4l2_buffer.sequence   the card's frame counter (capture) or the number
 *                          of frames sent before this one, repeats included
 *                          (output): a gap of n is n frames lost, or the
 *                          previous frame sent again n times
 *   v4l2_buffer.timestamp  CLOCK_MONOTONIC of the frame event (capture) or
 *                          of the moment the card took the frame (output)
 *   V4L2_BUF_FLAG_ERROR    the frame is not to be trusted
 *   G_DV_TIMINGS           the frame geometry (total and active lines)
 *   ENUMINPUT.status and V4L2_EVENT_SOURCE_CHANGE  the input state
 *   plane 2                the SMPTE 352 payload identifier, as an ANC packet
 *
 * What is left is what V4L2 has no place for. A card without a clock of its
 * own puts 0 in hw_timestamp; a card that does not check line CRCs puts 0 in
 * crc_errors.
 *
 * Output: the client fills magic, version, flags and audio_samples, the rest
 * zero (vendor tail included, unless the driver documents a block it takes);
 * a plane with another magic or version is not read and the frame goes
 * without it (the audio is halved between the fields). Of the flags a
 * playout node takes SDI_F_REC2020, SDI_F_HLG, SDI_F_PQ and SDI_F_LEVEL_B
 * as a statement about that one frame; the rest tell what was received and
 * mean nothing on the way out. A frame setting none of the three colour
 * flags goes out with the colorimetry and the transfer characteristic the
 * node itself is set to, and one without SDI_F_LEVEL_B with the 3G mapping
 * the node itself is set to, so a client that states nothing changes
 * nothing. A card that cannot change one of them between frames keeps its
 * own setting and says so in the driver's documentation. On return the
 * driver fills hw_timestamp with the card time at which it took the frame,
 * when the card has a clock.
 */
#define SDI_META_MAGIC		v4l2_fourcc('S', 'D', 'I', '0') /* an SDI frame by this contract */
#define SDI_META_VERSION	4
#define SDI_META_SIZE		128	/* the common part; the vendor block follows it */
#define SDI_META_RESERVED	18

#define SDI_F_LEVEL_B		(1u << 0) /* 3G input carried as SMPTE 425 level B (two streams in one link), by VPID */
#define SDI_F_PSF		(1u << 1) /* progressive segmented frame (SMPTE RP 211): two fields of one instant, not to be deinterlaced; by VPID */
#define SDI_F_RGB		(1u << 2) /* the source carried RGB 4:4:4 (SMPTE 372M/425M), not Y'CbCr 4:2:2; by VPID */
#define SDI_F_REC2020		(1u << 3) /* colorimetry Rec. 2020 (ST 352 colorimetry code 2, see below); clear: Rec. 709 or not stated */
#define SDI_F_HLG		(1u << 4) /* transfer characteristic HLG (ST 352 byte 2 bits 5..4 = 1) */
#define SDI_F_PQ		(1u << 5) /* transfer characteristic PQ, ST 2084 (ST 352 byte 2 bits 5..4 = 2); HLG and PQ never together, neither: SDR */
/*
 * Where ST 352 keeps the colorimetry code depends on the standard in byte 1:
 * in the 1080-line 1.5G standards and the dual links built from them (0x85,
 * 0x87, 0x8A, 0x96, 0x98) it is split over byte 3 bit 7 (high) and bit 4
 * (low), because byte 3 bit 5 is the 16:9 flag there; in every other
 * standard it is byte 3 bits 5..4, and bit 7 is the 16:9 flag.
 */

struct sdi_meta {
	__u32 magic;		/* 0:   SDI_META_MAGIC */
	__u32 version;		/* 4:   SDI_META_VERSION */
	__u32 flags;		/* 8:   SDI_F_* */
	__u32 crc_errors;	/* 12:  lines with a CRC error in this frame */
	__aligned_u64 hw_timestamp; /* 16: card clock, ns: start of the frame (capture), when the card took it (output) */
	__u32 audio_present;	/* 24:  mask of channels 0..15 found in this frame */
	__u32 audio_samples[2];	/* 28:  samples per channel in field 1 and 2 (progressive: all in [0]) */
	__u32 audio_rate;	/* 36:  rate from the audio control packets, 0 if unknown */
	__u32 audio_nonpcm;	/* 40:  channels carrying data (SMPTE 337M) rather than PCM */
	__u32 reserved[SDI_META_RESERVED]; /* 44: future common fields, zero */
	__u32 vendor_bytes;	/* 116: length of the vendor block right after the common part; 0 if none */
	__u32 vendor_magic;	/* 120: fourcc naming the card, chosen by the driver; 0 if it does not name itself */
	__u32 vendor_version;	/* 124: layout version of the vendor block */
};

#ifndef __cplusplus
_Static_assert(sizeof(struct sdi_meta) == SDI_META_SIZE,
	       "the common part of sdi_meta must be SDI_META_SIZE bytes");
#endif

#endif
