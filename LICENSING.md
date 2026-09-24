# Licensing

The driver is derived from the Linux driver of AVMatrix HWS cards at
https://github.com/benhoff/hws, Copyright (C) Ben Hoff and AVMatrix,
licensed GPL-2.0-only; its history is kept in this repository. What was
taken from it is the knowledge of the card: the register map, the start
sequence, the address windows and the channel layout. The V4L2 layer, the
frame contract, the polling, the slots and the audio placement are ours,
Copyright (C) 2026 Max Lapshin <max@flussonic.com>, and are licensed under
the same terms: the GNU General Public License version 2 only (`LICENSE`).
Each file says so in its `SPDX-License-Identifier` line, and the module
declares `MODULE_LICENSE("GPL")`.

`include/sdi_av.h` is the frame contract shared byte for byte with our SDI
drivers, and `include/hwav.h` the vendor block of this card; both are
GPL-2.0 WITH Linux-syscall-note -- the uapi exception, so a program of any
license may include them to speak to the nodes.
