# avmatrix-v4l2

Linux V4L2 driver for AVMatrix HWS PCIe capture cards (HWS X4 HDMI and
relatives). Every input of a card gets a multi-planar capture node
(`/dev/videoN`) whose buffers carry a whole frame: the picture, the frame's
audio and per-frame metadata, laid out by the frame contract our SDI
drivers follow -- `include/sdi_av.h`, the same file byte for byte in each of
them and in the client. So a program that captures from a DeckLink, a KONA
or a DekTec card captures from an HWS input with the same code; the
streamer's SDI input takes these nodes as they are. The vendor block
declared in `include/hwav.h` carries what only this card tells: the raster
the input arrived in and, on boards past version 121, the card's own frame
rate count and HDCP. What HDMI carries beyond picture and stereo sound --
InfoFrames, HDR, the audio format, CEC -- the card does not pass on
(`docs/hardware.md`).

The module is ours, written against the card's registers. What those
registers are and how the card has to be started comes from the driver of
github.com/benhoff/hws (GPL-2.0), whose history this repository keeps;
`docs/hardware.md` is what the bench showed on top of it. Only standard
Linux interfaces face the user: the V4L2 nodes, a media device per card
(`/dev/mediaN`: model, board version, which node is on which connector) and
counters in sysfs next to each node. There is no ALSA device: the audio of
an input travels in its frames, next to the picture it belongs to.

## Installing

The module ships as source for DKMS, so that every kernel the machine boots
gets its own build. On Debian and Ubuntu:

```sh
apt install ./hwsv4l2-dkms_26.09.0_all.deb
apt install ./hwsv4l2-dev_26.09.0_all.deb      # /usr/include/sdi_av.h and hwav.h, for clients
```

The package builds the module for the installed kernels (it needs their
headers: `linux-headers-generic` on Ubuntu, `linux-headers-amd64` on
Debian) and keeps `HwsCapture`, the other driver of these cards, from
autoloading, because whichever loads first owns them. If that module is
already loaded, unbind the card from it once
(`echo 0000:03:00.0 > /sys/bus/pci/drivers/HwsCapture/unbind`) or reboot.
Reboot rather than unload it on a machine that should keep working: at
unbind `HwsCapture` disables its interrupt line for good, and whatever
shares that line with it (an SMBus controller on the bench) goes quiet
until the next boot.

## Building from the tree

```sh
make                 # build/hwsv4l2.ko for the running kernel
make tools           # tools/hwav
make deb             # the two .deb (dpkg-buildpackage, debhelper, dh-dkms)
sudo make load       # loads the V4L2 dependencies and the module
```

`Dockerfile` is the build environment against a distribution kernel; the CI
builds the module against Debian 12, Debian 13, Ubuntu 24.04 and 26.04,
builds the package on Ubuntu 24.04 and installs it into a clean container,
where DKMS has to build the module with nothing but the package.

## Using the nodes

One capture node per input, `HDMI in n` (`SDI in n` on the SDI boards);
`media-ctl -p` shows which is which.

```sh
tools/hwav info /dev/video18                 # signal, detected and set timings
v4l2-ctl -d /dev/video18 --query-dv-timings
tools/hwav cap /dev/video18 -n 600 -a        # capture, print the audio of each frame
tools/hwav cap /dev/video18 -f yuyv -o /tmp/frames -n 3
tools/hwav cap /dev/video18 -t 1080p59.94 -n 5   # timings set, no detection
tools/hwav cap /dev/video18 -u -n 5          # USERPTR buffers
v4l2-ctl -d /dev/video18 --log-status        # input, counters into dmesg
cat /sys/class/video4linux/video18/signal
media-ctl -p -d /dev/media2
```

Planes of every buffer (`include/sdi_av.h` has the details):

| plane | content |
|---|---|
| 0 | picture: `SDUY` (UYVY) or `SDYU` (YUYV) |
| 1 | 16 channels x 32-bit samples, 48 kHz, interleaved: the input's stereo pair in channels 0 and 1, its 16-bit sample in the top bits; the other channels zero |
| 2 | empty: HDMI has no ancillary data packets, and the card passes on no InfoFrame |
| 3 | `struct sdi_meta` (magic `SDI0`, version 4, 128 bytes) followed by `struct hwav_meta` (`HWAV` in the vendor tail) |
| 4 | empty: no SD blanking lines |

The card delivers YUYV; UYVY, which the other cards deliver and the
streamer asks for, is the same picture with the bytes of every pair swapped
on the copy out of the driver's own buffers (the card cannot write into the
client's buffers, below). The picture of an input larger than 1920x1080
comes scaled down to it by the card, and `HWAV_F_SCALED` says so.

The audio of a frame is exactly what arrived during it: the card delivers
audio in packets of 1024 samples, and the driver places the ends of every
frame in that stream by their time -- 800 or 801 samples a frame at 59.94,
48 000 a second, none lost or given twice. `audio_nonpcm` marks a frame
whose pair carries an IEC 61937 stream (AC-3 and the like) instead of PCM.

The frame counter and its timestamp (CLOCK_MONOTONIC of the end of the
frame) are in `v4l2_buffer`, the geometry in `G_DV_TIMINGS`, the state of
the input in `ENUMINPUT.status` (`V4L2_IN_ST_NO_SIGNAL`, and
`V4L2_IN_ST_NO_ACCESS` for an HDCP-protected source) and
`V4L2_EVENT_SOURCE_CHANGE`. Brightness, contrast, saturation and hue are
the card's own and standard V4L2 controls.

### The frame rate

The card measures the raster of an input but counts its frame rate in whole
numbers, which cannot tell 60 from 59.94; the driver measures the rate
itself, over two seconds of frames, as soon as a signal appears.
`QUERY_DV_TIMINGS` answers `ENOLCK` until it has -- a client polls it the
way it waits for a signal -- and then the CEA-861 or DMT timings of that
raster and rate, a 1000/1001 rate as the whole rate's timings with
`V4L2_DV_FL_REDUCED_FPS`. Frames of another raster or rate than the timings
set are not delivered and count as `no_sync`; the rate changing under a
running capture raises `SOURCE_CHANGE`.

## How the card is driven

Two things about the card shape the driver; `docs/hardware.md` has the
measurements.

The card is **polled**, not interrupt driven. Its frame and audio done bits
are pulses that clear themselves some 30 us after they rise when the
bridge's interrupt enables are set, and most of them never reach the host
as interrupts, on the legacy line and over MSI alike: a 59.94 Hz input gave
25 to 40 interrupts a second. With the enables clear the bits latch, and
the driver reads them every 250 us while an input captures (`poll_us`
module parameter, 50 to 300: at 500 the card already loses a frame in ten).
The engine skips a frame whose predecessor's bit is still
up when it starts, so the poll is kept well inside the frame gap.

The engine writes each line of a frame to its buffer register's address
plus the line's offset, and reads the register as it goes; it cannot
scatter, and the next frame begins some tens of microseconds after the done
event -- no time to point the register elsewhere. So the driver moves the
register to a fresh slot of its own in the **middle** of every frame: a
frame lies in two slots, its top in the one it started in and its bottom in
the next, and the line where it crossed -- to the word -- is found from
markers the driver writes into a slot before handing it out. The two parts
are copied into the client's buffer, the bottom first, while the next frame
is still half a frame away from overwriting it.

## State

Brought up on an HWS X4 HDMI (8888:8504, board version 121.0) under
Ubuntu 24.04 with kernel 6.14, with a Roku player on input 3 sending
1080p59.94: the rate detected as 59.94, capture through MMAP and USERPTR in
UYVY and YUYV, 600-frame runs without a gap, a lost event or a split frame
unresolved, 48 000 audio samples a second; `v4l2-compliance -m` on the media
device and all four nodes without a failure or a warning (the streaming
part, `-s`, stops at the empty ANC and VBI planes, whose `bytesused` of 0 is
what the contract asks for). The streamer's SDI input captured it for
minutes as H.264 and AAC without a frame lost.

Under a debug kernel (6.14 with KASAN, lockdep, kmemleak, UBSAN bounds and
DMA API checks): five minutes of capture without a frame lost or a split
line unresolved, start/stop on all four nodes at once, timings refused
below 640x480 or of an odd width, the engine stopped under the watchdog
while STREAMOFF comes and goes, unbind and bind while idle and while
capturing, unbind with a node held open and closed afterwards -- no report,
no leak.

Not verified: interlaced inputs (the raster the card reports for them is
taken as one field and the frame as woven -- no source of ours sends one to
this card), inputs larger than 1080p, HDCP sources, the SDI boards of the
family and every board but the X4 HDMI, and the picture across the split
line against a source with motion in every line (the Roku sends a screen
saver).

## Counters

The names next to a node, their meaning and the rule for a counter the card
cannot report are the contract in `docs/sdi-sysfs.md`, shared with our SDI
drivers. A node carries `frames`, `frames_skipped`, `no_buffer`, `no_sync`,
`resyncs`, `events_missed`, `dma_errors`, `restarts` and `signal`, and one of
its own, `hdcp`, on boards past version 121 (the earlier ones do not report
it). `resyncs` counts frames given up because the poll moved the
register too late, `dma_errors` frames that did not arrive whole -- the
next frame reached the copy, or the split could not be placed. The card
counts no line CRCs, so there is no `crc_errors`.

## Licensing

GPL-2.0-only: the driver is derived from the GPL-2.0-only driver of
github.com/benhoff/hws, Copyright (C) Ben Hoff, and so is ours
(Copyright (C) 2026 Max Lapshin), `LICENSE`. `include/sdi_av.h` and
`include/hwav.h` carry the uapi syscall note, so a program of any license may
include them to speak to the nodes. See `LICENSING.md`.
