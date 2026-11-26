# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

`FTVIEW.WMF`: a Wild Commander plugin (C, SDCC) for TS-Conf with the FT812 (Karabas Go / Mini G,
ZX Evolution + VDAC2): JPG, PNG, DLS, DXP, AVI. The base is alex/ftview of Alex Korochinskiy
(git.alexkorochinskiy.ru, remote `upstream`; the ftview of `pentevo/sdk/ft812sdk` without
.XM / .XMZ, so it does not take the extensions of XMPLAY.WMF). The AVI player of FTView of Wild
Commander Improved v1.11i (github.com/andrewinsidelazarev/Wild-Commander-Improved,
`source/plugins/ftview`, MIT, Andrew Lazarev) is being ported here step by step: frame by frame
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

- `make` (tools/build.py): the SDCC of the system (4.2.0 here and in CI, Ubuntu 24.04), no Python
  packages. `make venv` makes `.venv` (intelhex, z80, Pillow) for the tests; `make test`.
- ABI: vendor/ libraries and the __naked functions of src/wc_api.c are ABI 0 (stack); the C code
  and the SDCC library ABI 1. build.py writes copies of the SDK headers with __sdcccall(0) into
  build/abi; a global --sdcccall 0 breaks memcmp and the 32-bit multiply. New __naked functions
  that read their arguments from the stack get __sdcccall(0).
- Layout: code from #8003, _HOME linked again right after _CODE (newer SDCC libs put code there,
  the linker placed it past _DATA), data from #A800 up to #C000, at most 32 blocks of 512 bytes a
  page; the header src/wc_h.c (512 bytes) first. crt0 does not run GSINIT: initialised non-const
  globals are not initialised.
- vendor/ is pentevo/sdk/ft812sdk/lib, checked against vendor/manifest.json; it is not edited.
- The build is reproducible; FTVIEW.WMF is committed and CI checks it is the build of the commit.

## Rules

- Commit messages, code comments and CLAUDE.md in English; README.md in Russian (it came so).
- Board check (Karabas Mini G, SD card, hwq ticket) before a release; the user tests himself on
  the board: ask first.
- Release: tag vX.Y.Z (CI publishes FTVIEW.WMF), then copy it to karabas-go `software/` by a PR
  and update docs/howto.md there.
