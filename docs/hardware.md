# What the HWS card was seen to do

Measured on an HWS X4 HDMI (PCI 8888:8504, subsystem 8888:0007, board
version 255.121) in a PCIe 2.0 x4 slot, Ubuntu 24.04, kernel 6.14, with a
Roku player sending 1080p59.94 to input 3. The register map is in
`hwsv4l2/hwsv4l2_regs.h`; what follows is what the driver's design rests on,
each point with the experiment behind it, so that a different board or a
different firmware can be checked against it.

## Done events are pulses, and interrupts lose them

The core's status word (BAR0 0x4004) has a video done bit per input
(bits 0-3) and an audio done bit per input (bits 8-11), cleared by writing
them back.

- With the bridge's interrupt enables (0x0134) set, a done bit clears itself
  some 30 us after it rises (a user-space loop reading 0x4004 saw every
  video done bit up for 21-52 us, 16.68 ms apart).
- Of those pulses few become interrupts: 25 to 40 a second from a
  59.94 Hz input, at irregular multiples of the frame period. The same over
  the legacy line and over MSI, with any of the three enable bits that
  exist (8, 16, 17 -- the other bits of 0x0134 do not hold a write), and
  whatever the handler wrote after acknowledging (enable toggling, 0x0138,
  0x0148).
- With the enables clear the bits latch until written back. Read every
  250 us, every video done event of the input was seen: 16.68 ms apart,
  59.94 a second.
- The engine skips a frame whose predecessor's done bit is still up when
  the frame starts: read every 500 us, one frame in ten was missing; every
  250 us and every 100 us, none.

Hence the driver polls, at 4 kHz, while an input captures.

## No blanking to speak of between frames

- The engine writes a frame as it arrives, in bursts about 30 us apart
  (the DMA busy bit, bit 3 of 0x4000, toggles at that pace through the
  whole frame).
- Row 0 of a frame marked right at the done event was overwritten 50 to
  100 us later in about a quarter of the frames; in the rest the new frame
  had already written row 0 before the mark. The next frame starts within
  some tens of microseconds of the done event.
- Moving the buffer register right at the done event (read every 50 or
  250 us) left 4 to 14 % of frames starting in the old buffer.

## The engine reads its buffer register as it goes

- A frame always lands at the buffer register's address, whatever the
  "half size" register (word 50 + input) says: with a two-frame buffer and
  the half size set to a whole frame, the second frame never appeared in the
  second half; the toggle register (word 32 + input) stayed 0 throughout.
  The half size set to exactly half a frame or to half rounded down to 2 KiB
  gave one done event per frame, not two.
- The register moved in the middle of a frame sends the rest of that frame
  to the new address, at the same line offsets: every frame's bottom
  landed in the slot handed out halfway through it.
- The line at which a frame crosses from one slot to the next is within
  some 40 lines of where the time of the move puts it (553-592 for a move
  at half a 1080-line frame, with the poll's own jitter), and the crossing
  falls inside a line in most frames (560 of 600): the line's first part in
  the old slot, the rest in the new.

Hence the driver's slots: the register moves in the middle of every frame,
a slot is marked at the start of every line and in every word of the lines
around the expected crossing, and a frame is put together from two slots.

## Frame rate

- The input frame rate register (word 110 + input) read 0 for the Roku
  input; the output rate register (word 130 + input) read 1 and did not
  take a write. The rate is the driver's measurement.
- The raster register (word 90 + 2 x input) gave 1920x1080 as soon as the
  signal was there.

## Audio

- One packet is 1024 stereo frames of 16-bit samples, 4 KiB; the engine
  fills a ring of two such packets, and the toggle register (word 40 +
  input) names the one being written. 46.87 packets a second, 48 000
  samples: the audio clock of the Roku input against the host clock over a
  10-second capture came out at 47 999.5 to 48 000.5.

## Starting and stopping

- The start sequence of the vendor's driver is kept as it is (decoder mode
  0, 0x10, the address windows, 0x80000000, 0x80ffffff, 0x13); nothing else
  was tried.
- The vendor's driver disables its interrupt line at unbind and never
  enables it again; the line of the bench was shared with an SMBus and an
  I2C controller, which went quiet with it.
