// SDRTEST.WMF: board check of the SD reader (src/sdr.c). Started on a file
// (.SDT, .MP4, .MKV), it opens the file with sdr_open, reads it with
// sdr_read and with LOAD512 of WC, compares the two and shows the result:
// the card, the volume, the extents, the speed of both ways.
#include <stdio.h>
#include <string.h>
#include <defs.h>
#include <wc_api.h>
#include "sdr.h"
#include <wc_api.c>

#define TMN (*(volatile u8*)0x6009)
#define PAGE3 (*(volatile u8*)0x6003)
#define WINDOW ((u8*)0xC000)
#define SPEED_SECTORS 16384UL     // the speed passes read up to 8 MB

u16 ret_sp;
u32 filesize;
u8 file_ext;
u8 call_type;
u8 buf[4096];
char text[640];

const char *const names[] = {"OK", "EMPTY", "SAMPLE", "CHAIN", "CARD", "VOLUME",
                             "ALIGN", "FAT", "FRAG", "VERIFY", "IO"};

WC_TX_WINDOW win =
{
  WC_WIND_HDR_TXT, 0, 18, 6, 44, 17, 0x4F, 0, 0, 0, 0,
  (u16*)"\x0E" " SD reader test ",
  (u16*)"\x0E" "any key",
  (u16*)text,
};

// Buffer page 1 of this plugin into #C000 (API 0, A' = 1).
void map_buffer() __naked
{
  __asm
    ld a, #1
    ex af, af
    xor a
    jp _WCAPI
  __endasm;
}

// Show / remove the window: the WC calls take it in IX, the frame pointer.
void win_show() __naked
{
  __asm
    push ix
    ld ix, #_win
    ld a, #_PRWOW
    call _WCAPI
    pop ix
    ret
  __endasm;
}

void win_hide() __naked
{
  __asm
    push ix
    ld ix, #_win
    ld a, #_RRESB
    call _WCAPI
    pop ix
    ret
  __endasm;
}

u16 kbs(u32 sectors, u32 frames)
{
  return frames ? sectors * 25 / frames : 0;    // 512 B * 50 Hz / 1 KB
}

void run()
{
  u8 r, t, n, page;
  u32 sec, nsec = (filesize + 511) >> 9, lim;
  u32 f_sdr = 0, f_wc = 0, bad = 0xFFFFFFFFUL, cmp_done = 0;
  u8 f_open;
  bool io_ok = true, stopped = false;
  u16 len;
  char *p = text;

  t = TMN;
  r = sdr_open(buf, filesize);
  f_open = TMN - t;
  p += sprintf(p, "\x0D" " size %lu, result %s", filesize, names[r]);
  if (r == SDR_OK)
  {
    p += sprintf(p, "\x0D" " card SD%c, %s, %u sect/cluster",
                 sdr_cs == 1 ? '1' : '2', sdr_hc ? "SDHC" : "SDSC", sdr_spc);
    p += sprintf(p, "\x0D" " %lu clusters, %u extents, open %u ms",
                 sdr_ncl, sdr_next, f_open * 20);
    map_buffer();
    page = PAGE3;
    lim = nsec < SPEED_SECTORS ? nsec : SPEED_SECTORS;
    // speed of the reader
    for (sec = 0; sec < lim && !stopped; sec += n)
    {
      n = lim - sec < 32 ? lim - sec : 32;
      t = TMN;
      if (!sdr_read(sec, n, page, 0)) { io_ok = false; break; }
      f_sdr += (u8)(TMN - t);
      stopped = wc_api__bool(_ESC);
    }
    // speed of LOAD512
    wc_api__bool(_GIPAGPL);
    for (sec = 0; sec < lim && !stopped; sec += n)
    {
      n = lim - sec < 32 ? lim - sec : 32;
      t = TMN;
      wc_load512(WINDOW, n);
      f_wc += (u8)(TMN - t);
      stopped = wc_api__bool(_ESC);
    }
    // the whole file, both ways
    wc_api__bool(_GIPAGPL);
    for (sec = 0; sec < nsec && io_ok && !stopped; sec += n)
    {
      n = nsec - sec < 8 ? nsec - sec : 8;
      wc_load512(buf, n);
      if (!sdr_read(sec, n, page, 0)) { io_ok = false; break; }
      len = (filesize - (sec << 9)) < ((u16)n << 9) ? filesize - (sec << 9) : (u16)n << 9;
      if (memcmp(WINDOW, buf, len))
      {
        for (bad = 0; memcmp(WINDOW + (bad << 9), buf + (bad << 9), 512); bad++);
        bad += sec;
        break;
      }
      cmp_done = sec + n;
      stopped = wc_api__bool(_ESC);
    }
    wc_api__bool(_GIPAGPL);
    p += sprintf(p, "\x0D\x0D" " reader  %u KB/s", kbs(lim, f_sdr));
    p += sprintf(p, "\x0D" " LOAD512 %u KB/s  (first %lu KB)", kbs(lim, f_wc), lim >> 1);
    if (!io_ok)
      p += sprintf(p, "\x0D\x0D" " READ ERROR at sector %lu", sec);
    else if (bad != 0xFFFFFFFFUL)
      p += sprintf(p, "\x0D\x0D" " MISMATCH at sector %lu", bad);
    else
      p += sprintf(p, "\x0D\x0D" " compared %lu of %lu sectors: %s", cmp_done, nsec,
                   cmp_done == nsec ? "EQUAL" : "stopped");
    p += sprintf(p, "\x0D" " LBA of sector 0: %lu", sdr_lba(0));
  }
  win_show();
  wc_api__bool(_USPO);
  wc_api__bool(_NUSP);
  win_hide();
}

void main()
{
  __asm
    ld (_ret_sp), sp
    ld (_call_type), a
    ex af, af
    ld (_file_ext), a
    ld (_filesize), hl
    ld (_filesize + 2), de
  __endasm;
  if (call_type == WC_CALL_EXT)
    run();
  wc_exit(WC_EXIT);
}
