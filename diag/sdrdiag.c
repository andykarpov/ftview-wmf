// SDRTEST.WMF: board check of the SD reader (src/sdr.c). Started on a file
// (.SDT, .MP4, .MKV), it opens the file with sdr_open, reads it with
// sdr_read and with LOAD512 of WC, compares the two and shows the result:
// the card, the volume, the extents, the speed of both ways.
#include <stdio.h>
#include <string.h>
#include <defs.h>
#include <wc_api.h>
#include "sdr.h"
#include "wc_api_min.c"

#define TMN (*(volatile u8*)0x6009)
#define PAGE3 (*(volatile u8*)0x6003)
#define WINDOW ((u8*)0xC000)
#define SPEED_SECTORS 4096UL      // the speed passes read up to 2 MB

u16 ret_sp;
u32 filesize;
u8 file_ext;
u8 call_type;
u8 buf[2048];
char text[640];

const char *const names[] = {"OK", "EMPTY", "SAMPLE", "CHAIN", "CARD", "VOLUME",
                             "ALIGN", "FAT", "FRAG", "VERIFY", "IO"};

// const: crt0 does not copy initialisers (GSINIT), a non-const window would
// be zeros. PRWOW writes into it: the page is RAM, the casts drop the const.
// The texts are named arrays: SDCC 4.2 gave the string literals of two const
// structs the same labels.
const char t_title[] = "\x0E" " SD reader test ";
const char t_busy_ftr[] = "\x0E" "Esc stops";
const char t_busy[] = "\x0E\x0C\x01" "reading the file...";
const char t_ftr[] = "\x0E" "any key";

const WC_TX_WINDOW busy =
{
  WC_WIND_HDR_TXT, 0, 28, 14, 24, 5, 0x4F, 0, 0, 0, 0,
  (u16*)t_title, (u16*)t_busy_ftr, (u16*)t_busy,
};

const WC_TX_WINDOW win =
{
  WC_WIND_HDR_TXT, 0, 18, 6, 44, 17, 0x4F, 0, 0, 0, 0,
  (u16*)t_title, (u16*)t_ftr, (u16*)text,
};

// Show / remove the window: the WC calls take it in IX, the frame pointer.
WC_TX_WINDOW *win_p;

void win_call(u8 fn) __naked
{
  fn;
  __asm
    push ix
    ld ix, (_win_p)
    call _WCAPI
    pop ix
    ret
  __endasm;
}

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

// A line of the busy window: PRSRW (API 3), IX window, HL text, D y, E x,
// BC length.
char line[24];

void win_line() __naked
{
  __asm
    push ix
    ld ix, #_busy
    ld hl, #_line
    ld de, #0x0201
    ld bc, #22
    ld a, #_PRSRW
    call _WCAPI
    pop ix
    ret
  __endasm;
}

u8 last_tick;

// Stage and sector in the busy window, at most a few times a second.
void progress(char stage, u32 sec, u32 total)
{
  u8 n;
  if ((u8)(TMN - last_tick) < 10) return;
  last_tick = TMN;
  n = sprintf(line, "%c %lu/%lu", stage, sec, total);
  while (n < sizeof(line) - 2) line[n++] = ' ';
  win_line();
  map_buffer();          // PRSRW left the text page of WC in #C000
}

void win_show(WC_TX_WINDOW *w) { win_p = w; win_call(_PRWOW); }
void win_hide(WC_TX_WINDOW *w) { win_p = w; win_call(_RRESB); }

// Z80 clock (TURBOPL, API 14): c = 0 3.5 MHz, 2 14 MHz. The player runs at
// 14 MHz, so the speeds are measured there; WC 1.11 rc6 has no call that
// restores its own setting, the exit sets 3.5 MHz (CPU_FREQ=0 of the bench).
void cpu_clock(u8 c) __naked
{
  c;
  __asm
    push ix
    ld c, a
    ld b, #0
    ld a, #_TURBOPL
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

  wc_api__bool(_GEDPL);     // text mode, palette, offsets: required at the start
  win_show((WC_TX_WINDOW*)&busy);
  cpu_clock(2);
  t = TMN;
  r = sdr_open(buf, filesize);
  f_open = TMN - t;
  progress('O', r, 0);
  p += sprintf(p, "\x0D" " size %lu, result %s", filesize, names[r]);
  if (r == SDR_OK)
  {
    p += sprintf(p, "\x0D" " card SD%c, %s, %u sect/cluster",
                 sdr.cs == 1 ? '1' : '2', sdr.hc ? "SDHC" : "SDSC", sdr.spc);
    p += sprintf(p, "\x0D" " %lu clusters, %u extents, open %u ms",
                 sdr.ncl, sdr.next, f_open * 20);
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
      progress('R', sec, lim);
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
      progress('W', sec, lim);
      stopped = wc_api__bool(_ESC);
    }
    // the whole file, both ways
    wc_api__bool(_GIPAGPL);
    for (sec = 0; sec < nsec && io_ok && !stopped; sec += n)
    {
      n = nsec - sec < 4 ? nsec - sec : 4;
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
      progress('C', sec, nsec);
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
  cpu_clock(0);
  win_hide((WC_TX_WINDOW*)&busy);
  win_show((WC_TX_WINDOW*)&win);
  wc_api__bool(_USPO);
  wc_api__bool(_NUSP);
  win_hide((WC_TX_WINDOW*)&win);
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
