// SPDX-License-Identifier: GPL-2.0-only
/*
 * hwav: bench tool for the hwsv4l2 nodes.
 *
 *   hwav info /dev/videoN              input status, detected and set timings
 *   hwav cap /dev/videoN [-n N] [-f FMT] [-o DIR] [-a] [-u] [-t WxH@FPS]
 *                                      capture N frames; -a prints the audio of
 *                                      each frame (samples, peaks of both
 *                                      channels), -o writes every plane of every
 *                                      frame, -f uyvy|yuyv picks the pixel
 *                                      format, -u uses USERPTR buffers instead of
 *                                      MMAP, -t sets the timings instead of
 *                                      querying them (1080p60, 1080p59.94, ...)
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/v4l2-dv-timings.h>

#include "../include/hwav.h"

#define NBUF 6
#define NPLANES SDI_NUM_PLANES

struct buf {
	void *p[NPLANES];
	size_t len[NPLANES];
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

/*
 * Frames a second of the timings: REDUCED_FPS on a whole rate at the nominal
 * clock is that rate times 1000/1001, the way v4l2_calc_timeperframe() reads
 * it.
 */
static double bt_frame_rate(const struct v4l2_bt_timings *bt)
{
	uint64_t htot = V4L2_DV_BT_FRAME_WIDTH(bt), vtot = V4L2_DV_BT_FRAME_HEIGHT(bt);
	double fps = htot && vtot ? (double)bt->pixelclock / (htot * vtot) : 0;

	if ((bt->flags & V4L2_DV_FL_REDUCED_FPS) && fabs(fps - round(fps)) < 0.01)
		fps = fps * 1000 / 1001;
	return fps;
}

static void print_timings(const struct v4l2_dv_timings *t)
{
	const struct v4l2_bt_timings *bt = &t->bt;

	printf("%ux%u%c %.3f fps (pixelclock %llu)\n", bt->width, bt->height,
	       bt->interlaced ? 'i' : 'p', bt_frame_rate(bt), (unsigned long long)bt->pixelclock);
}

static int cmd_info(int fd)
{
	struct v4l2_input in = { .index = 0 };
	struct v4l2_dv_timings t;
	struct v4l2_capability cap;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
		printf("%s / %s\n", cap.card, cap.bus_info);
	if (xioctl(fd, VIDIOC_ENUMINPUT, &in) == 0)
		printf("input %s: status 0x%x%s%s\n", in.name, in.status,
		       in.status & V4L2_IN_ST_NO_SIGNAL ? " (no signal)" : "",
		       in.status & V4L2_IN_ST_NO_ACCESS ? " (HDCP)" : "");
	memset(&t, 0, sizeof(t));
	if (xioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &t) == 0) {
		printf("detected: ");
		print_timings(&t);
	} else {
		printf("detected: none (%s)\n", strerror(errno));
	}
	memset(&t, 0, sizeof(t));
	if (xioctl(fd, VIDIOC_G_DV_TIMINGS, &t) == 0) {
		printf("configured: ");
		print_timings(&t);
	}
	return 0;
}

/* "1080p59.94", "720p50": a CEA-861 preset, 1000/1001 rates as REDUCED_FPS. */
static int forced_timings(const char *name, struct v4l2_dv_timings *t)
{
	static const struct { const char *name; struct v4l2_dv_timings t; } table[] = {
		{ "1080p60", V4L2_DV_BT_CEA_1920X1080P60 },
		{ "1080p59.94", V4L2_DV_BT_CEA_1920X1080P60 },
		{ "1080p50", V4L2_DV_BT_CEA_1920X1080P50 },
		{ "1080p30", V4L2_DV_BT_CEA_1920X1080P30 },
		{ "1080p29.97", V4L2_DV_BT_CEA_1920X1080P30 },
		{ "1080p25", V4L2_DV_BT_CEA_1920X1080P25 },
		{ "1080i50", V4L2_DV_BT_CEA_1920X1080I50 },
		{ "1080i59.94", V4L2_DV_BT_CEA_1920X1080I60 },
		{ "720p60", V4L2_DV_BT_CEA_1280X720P60 },
		{ "720p59.94", V4L2_DV_BT_CEA_1280X720P60 },
		{ "720p50", V4L2_DV_BT_CEA_1280X720P50 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (!strcmp(name, table[i].name)) {
			*t = table[i].t;
			if (strstr(name, ".94") || strstr(name, ".97"))
				t->bt.flags |= V4L2_DV_FL_REDUCED_FPS;
			return 0;
		}
	fprintf(stderr, "unknown timings %s\n", name);
	return -1;
}

/* Peak of a channel of the plane, as a fraction of full scale. */
static double peak(const int32_t *s, unsigned int samples, unsigned int ch)
{
	int64_t m = 0;
	unsigned int i;

	for (i = 0; i < samples; i++) {
		int64_t v = llabs((int64_t)(s[i * SDI_AUDIO_CHANNELS + ch] >> 8));

		if (v > m)
			m = v;
	}
	return m / 8388608.0;
}

static int cmd_cap(int fd, int argc, char **argv)
{
	int n = 10, opt, i, ret = 1;
	uint32_t fmt = SDI_PIX_FMT_UYVY;
	const char *out = NULL, *forced = NULL;
	int show_audio = 0, userptr = 0;
	uint64_t first_ts = 0, last_ts = 0, ts, samples_total = 0;
	unsigned int first_seq = 0, last_seq = 0, errors = 0;
	int first_set = 0;
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct buf bufs[NBUF];
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	optind = 1;
	while ((opt = getopt(argc, argv, "n:f:o:at:u")) != -1) {
		switch (opt) {
		case 'n': n = atoi(optarg); break;
		case 'f':
			if (!strcmp(optarg, "yuyv"))
				fmt = SDI_PIX_FMT_YUYV;
			break;
		case 'o': out = optarg; break;
		case 'a': show_audio = 1; break;
		case 't': forced = optarg; break;
		case 'u': userptr = 1; break;
		default: return 1;
		}
	}

	{
		struct v4l2_dv_timings t;

		memset(&t, 0, sizeof(t));
		if (forced) {
			if (forced_timings(forced, &t) < 0)
				return 1;
		} else if (xioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &t) < 0) {
			fprintf(stderr, "no timings: %s\n", strerror(errno));
			return 1;
		}
		if (xioctl(fd, VIDIOC_S_DV_TIMINGS, &t) < 0) {
			fprintf(stderr, "S_DV_TIMINGS: %s\n", strerror(errno));
			return 1;
		}
		printf("capturing ");
		print_timings(&t);
	}

	memset(&f, 0, sizeof(f));
	f.type = type;
	f.fmt.pix_mp.pixelformat = fmt;
	if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) {
		fprintf(stderr, "S_FMT: %s\n", strerror(errno));
		return 1;
	}
	printf("format %.4s, planes:", (char *)&f.fmt.pix_mp.pixelformat);
	for (i = 0; i < (int)f.fmt.pix_mp.num_planes; i++)
		printf(" %u", f.fmt.pix_mp.plane_fmt[i].sizeimage);
	printf(" bytes\n");

	memset(&rb, 0, sizeof(rb));
	rb.count = NBUF;
	rb.type = type;
	rb.memory = userptr ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) {
		fprintf(stderr, "REQBUFS: %s\n", strerror(errno));
		return 1;
	}
	memset(bufs, 0, sizeof(bufs));
	for (i = 0; i < (int)rb.count; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane pl[NPLANES];
		int k;

		memset(&b, 0, sizeof(b));
		memset(pl, 0, sizeof(pl));
		b.type = type;
		b.memory = rb.memory;
		b.index = i;
		b.m.planes = pl;
		b.length = NPLANES;
		if (userptr) {
			for (k = 0; k < NPLANES; k++) {
				bufs[i].len[k] = f.fmt.pix_mp.plane_fmt[k].sizeimage;
				if (posix_memalign(&bufs[i].p[k], 4096, bufs[i].len[k])) {
					fprintf(stderr, "posix_memalign failed\n");
					return 1;
				}
				memset(bufs[i].p[k], 0, bufs[i].len[k]);
				pl[k].m.userptr = (unsigned long)bufs[i].p[k];
				pl[k].length = bufs[i].len[k];
			}
		} else {
			if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
				fprintf(stderr, "QUERYBUF: %s\n", strerror(errno));
				return 1;
			}
			for (k = 0; k < NPLANES; k++) {
				bufs[i].len[k] = pl[k].length;
				bufs[i].p[k] = mmap(NULL, pl[k].length, PROT_READ, MAP_SHARED, fd,
						    pl[k].m.mem_offset);
				if (bufs[i].p[k] == MAP_FAILED) {
					fprintf(stderr, "mmap: %s\n", strerror(errno));
					return 1;
				}
			}
		}
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			fprintf(stderr, "QBUF: %s\n", strerror(errno));
			return 1;
		}
	}
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "STREAMON: %s\n", strerror(errno));
		return 1;
	}

	for (i = 0; i < n; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane pl[NPLANES];
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		const struct sdi_meta *meta;
		const struct hwav_meta *hw;

		if (poll(&pfd, 1, 3000) <= 0) {
			fprintf(stderr, "frame %d: timeout\n", i);
			goto out;
		}
		memset(&b, 0, sizeof(b));
		memset(pl, 0, sizeof(pl));
		b.type = type;
		b.memory = rb.memory;
		b.m.planes = pl;
		b.length = NPLANES;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			fprintf(stderr, "DQBUF: %s\n", strerror(errno));
			goto out;
		}
		meta = bufs[b.index].p[SDI_PLANE_META];
		if (meta->magic != SDI_META_MAGIC || meta->version != SDI_META_VERSION ||
		    meta->vendor_magic != HWAV_VENDOR_MAGIC ||
		    meta->vendor_version != HWAV_VENDOR_VERSION ||
		    meta->vendor_bytes < sizeof(*hw)) {
			fprintf(stderr, "frame %d: metadata magic 0x%08x version %u vendor 0x%08x/%u/%u\n",
				i, meta->magic, meta->version, meta->vendor_magic,
				meta->vendor_version, meta->vendor_bytes);
			goto out;
		}
		hw = (const struct hwav_meta *)((const uint8_t *)meta + SDI_META_SIZE);
		ts = (uint64_t)b.timestamp.tv_sec * 1000000000ULL + (uint64_t)b.timestamp.tv_usec * 1000;
		if (!first_set) {
			first_ts = ts;
			first_seq = b.sequence;
			first_set = 1;
		}
		last_ts = ts;
		last_seq = b.sequence;
		samples_total += meta->audio_samples[0];
		if (b.flags & V4L2_BUF_FLAG_ERROR)
			errors++;
		printf("frame %u seq %u%s audio %u samples%s video %u bytes, input %ux%u %s%s\n",
		       i, b.sequence, b.flags & V4L2_BUF_FLAG_ERROR ? " ERROR" : "",
		       meta->audio_samples[0], meta->audio_nonpcm ? " (data)" : "",
		       pl[SDI_PLANE_VIDEO].bytesused, hw->in_width, hw->in_height,
		       hw->flags & HWAV_F_HDCP ? " HDCP" : "",
		       hw->flags & HWAV_F_SCALED ? " scaled" : "");
		if (show_audio && meta->audio_samples[0])
			printf("  audio: present 0x%04x, peak L %.4f R %.4f\n", meta->audio_present,
			       peak(bufs[b.index].p[SDI_PLANE_AUDIO], meta->audio_samples[0], 0),
			       peak(bufs[b.index].p[SDI_PLANE_AUDIO], meta->audio_samples[0], 1));
		if (out) {
			char path[512];
			int k;

			for (k = 0; k < NPLANES; k++) {
				FILE *fp;

				snprintf(path, sizeof(path), "%s/frame%04d.p%d", out, i, k);
				fp = fopen(path, "wb");
				if (!fp)
					continue;
				fwrite(bufs[b.index].p[k], 1, pl[k].bytesused, fp);
				fclose(fp);
			}
		}
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			fprintf(stderr, "QBUF: %s\n", strerror(errno));
			goto out;
		}
	}
	ret = 0;
	if (last_seq > first_seq && last_ts > first_ts) {
		double s = (last_ts - first_ts) / 1e9;

		printf("captured %d frames over %u card frames in %.3f s: %.3f fps delivered, %.1f audio samples/s, %u errors\n",
		       n, last_seq - first_seq, s, (n - 1) / s, samples_total / s, errors);
	}
out:
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	return ret;
}

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc < 3) {
		fprintf(stderr, "usage: %s info|cap /dev/videoN [options]\n", argv[0]);
		return 2;
	}
	fd = open(argv[2], O_RDWR);
	if (fd < 0) {
		perror(argv[2]);
		return 1;
	}
	if (!strcmp(argv[1], "info"))
		ret = cmd_info(fd);
	else if (!strcmp(argv[1], "cap"))
		ret = cmd_cap(fd, argc - 2, argv + 2);
	else {
		fprintf(stderr, "unknown command %s\n", argv[1]);
		ret = 2;
	}
	close(fd);
	return ret;
}
