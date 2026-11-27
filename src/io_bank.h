#pragma once
// The I/O bank (page 10 of the WMF): the SD reader (sdr.c) and the set-up of
// the FT812 for the other banks, called by far_call(IO_PAGE, IO_FAR) with buffer page 1 in #C000
// (the reader state is in its tail, SDR_STATE_ADDR).

#define IO_PAGE 10
#define IO_OPEN 1         // sdr_open(fs_buf, size): the file WC started the plugin with
#define IO_READ 2         // sdr_read(fsec, n, page, offs)
#define IO_FT_INIT 3      // the FT812 in the mode of option n of -mode: ok, offs = width, fsec = height
#define IO_VIEW 4         // the frame of AVI_FAR (#FF00) fitted on the screen of the mode (view_w ...)

typedef struct
{
  u8 op, ok;
  u8 n, page;
  u16 offs;
  u32 fsec;
  u32 size;
} IO_SHARED;

// The tail of buffer page 1, after AVI_FAR (#FF00, 92 bytes), ESP_WFX (#FF60) and ESP_FAR (#FFC0).
#define IO_FAR ((IO_SHARED*)0xFFF0)
