# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

`FTVIEW.WMF`: a Wild Commander plugin (C, SDCC) for TS-Conf with the FT812 (Karabas Go / Mini G,
ZX Evolution + VDAC2): JPG, PNG, DLS, DXP, AVI. The base is alex/ftview of Alex Korochinskiy
(git.alexkorochinskiy.ru, remote `upstream`; the ftview of `pentevo/sdk/ft812sdk` without
.XM / .XMZ, so it does not take the extensions of XMPLAY.WMF). The AVI player of FTView of Wild
Commander Improved v1.11i (github.com/andrewinsidelazarev/Wild-Commander-Improved,
`source/plugins/ftview`, MIT, Andrew Lazarev; LICENSE.WCI) is ported here: frame by frame
playback with CMD_VIDEOFRAME, the seek table (idx1 or the FTVIEWI1 JUNK chunk of FTViewConvert),
the HUD, PCM8 on General Sound, the list of videos. Its code calls WC internals by address
(CURIT #44A5, GIPAG #44D7, SECBU #3000, CUHL #38A8, ...) taken from WC 1.11i; each of them is
not used here: the WC of the users (stock WC 1.11 rc6 on the bench) is not changed, and the
plugin uses only the public API of WC.

## SD reader (src/sdr.c)

The AVI player reads the file straight from the card, so it can seek without WC internals:
- sdr_open: the first sector through WC (GIPAGPL + LOAD512) and the LBAs of the first cluster
  (CHTOSEP, API 52: it writes whole clusters, then checks the end, so the end is the buffer + 4);
  the card is the one of SD1 / SD2 (#77 = %00000001 / %00001011, none %00000011, as SELDEV of the
  WC SD driver) that answers CMD58 as initialised and returns that sector; MBR / BPB of the FAT32
  volume; the chain walked in the FAT by the reader (fat_run: asm, contiguous links of a cached
  FAT sector) into extents (SDR_MAX_EXT), checked against the CHTOSEP list. WC's stream is left at
  the start of the file.
- sdr_read: CMD18 + DMA SPI -> RAM (DMACtrl #42, 256 bursts of a word, DMANUMH 0) into a page,
  CMD12 at the end of a run of sectors that follow on the card.
- Port #57 (zports.v SDDAT): a write starts an exchange with its byte, a read returns the byte of the
  previous exchange and starts one with #FF; a DMA byte is a read.
- The WC API calls keep IX / IY (IX is the frame pointer of the C code). The helpers of wc_api.c
  that take a window in IX (wc_api_u16 with PRWOW / RRESB) destroy it: only right before wc_exit.

## Tests

`make test` (seconds, `.venv` python): test/tsmodel.py is the Z80 (package z80) with the Z-Controller
SPI, SD cards in SPI mode, the DMA and the WC API calls; test/fatimg.py makes FAT32 images with
mkfs.fat / mtools (whole card or MBR partition, any cluster size), reads them independently and
scatters a file into runs. test_sdr.py runs test/sdr_main.c (sdr_open + reads) against them: SD1 /
SD2, SDHC / SDSC, fragmented files, idle card, wrong card, bad link, too many extents, the walk time
of 16384 clusters. test_diag.py runs the board check plugin.

`make diag`: build/SDRTEST.WMF (diag/sdrdiag.c, extensions SDT MP4 MKV, a buffer page): on the board
it opens the file under the cursor with the reader, reads it with the reader and with LOAD512, shows
the card, volume, extents, both speeds and the comparison of the whole file. Not released.

## Build

- `make` (tools/build.py): SDCC 4.3 or newer (WCI's code hits internal errors and duplicate labels
  of 4.2): `$SDCC_HOME`, default `~/.local/opt/sdcc-4.4.0` (the 4.4.0 tarball of sourceforge; CI
  downloads the same). `make venv` makes `.venv` (intelhex, z80, Pillow) for the tests; `make test`.
- ABI: vendor/ libraries and the __naked functions of src/wc_api.c are ABI 0 (stack); the C code
  and the SDCC library ABI 1. build.py writes copies of the SDK headers with __sdcccall(0) into
  build/abi; a global --sdcccall 0 breaks memcmp and the 32-bit multiply. New __naked functions
  that read their arguments from the stack get __sdcccall(0).
- Banks (BANKS of build.py, 11 pages of 512-byte blocks: defs [0,32, 9,32, 10,32, 1,0]):
  image (page 0: src/main.c, images, the AVI header, seek table, message windows), video (page 9:
  src/main.c with FTVIEW_VIDEO_BANK, the player, hud_cmd from #BB80), io (page 10: io_bank.c +
  sdr.c, the SD reader), buffer pages 1..8. Each bank: code #8006.., DATA #BD00..#BF80, the gates
  at #BF80 (wc_video_bank, _far_gate: the same bytes in every bank, checked), the entry frame
  #BFE0..#BFED (fixed `__at` addresses, checked). far_call(page, param) switches the bank by
  MNG8_PL (API 79, not #79: the SDK header has a typo) and back. The reader state (SDR_STATE,
  256 bytes) is at #FE00 of buffer page 1; IO_SHARED (the request) at #FFF0 of it.
- The WC page pool is about 63 pages: the plugin takes 11 (62 on the bench with the others).
- crt0 does not run GSINIT: initialised non-const globals are zeros (build.py fails on
  `_INITIALIZER`); make tables const.
- vendor/ is pentevo/sdk/ft812sdk/lib, checked against vendor/manifest.json; it is not edited.
- The build is reproducible; FTVIEW.WMF is committed and CI checks it is the build of the commit.

## AVI player (WCI)

- Frame by frame (CMD_VIDEOFRAME, media FIFO at RAM_G #E0000, the seek table at #D0000) only for
  MJPEG with PCM 8 bit or no audio; other audio (IMA ADPCM) goes to CMD_PLAYVIDEO (no HUD, no
  seek, Esc exits). PCM8 is played by the GS (src/gsdrv.s, assembled into gsdrv.h).
- The file is read by the SD reader (avi.io, avi.disk = sector); without it (open failed) LOAD512
  from the start, no seek. The list of videos (Enter) of WCI is not built: it reads the directory
  through WC internals.
- unreal-ng (emu-shot) runs the plugin on WC rc6 (images, keys) but its FT812 crashes on every
  AVI (the old FTVIEW too): AVI only on the board.

## Rules

- Commit messages, code comments and CLAUDE.md in English; README.md in Russian (it came so).
- Board check (Karabas Mini G, SD card, hwq ticket) before a release; the user tests himself on
  the board: ask first.
- Release: tag vX.Y.Z (CI publishes FTVIEW.WMF), then copy it to karabas-go `software/` by a PR
  and update docs/howto.md there.
