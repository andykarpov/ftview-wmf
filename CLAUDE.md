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
checked against the WC we ship (karabas-go `software/`) before use.

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
