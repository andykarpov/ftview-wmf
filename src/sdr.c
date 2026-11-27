// SD reader, see sdr.h.
//
// Port #57: a write starts an SPI exchange with that byte; a read returns
// the byte of the previous exchange and starts a new one with #FF (zports.v,
// SDDAT). A DMA SPI -> RAM byte is the same as a read. So a command is sent
// by writes only, its answer is taken by reads, and the read that returns
// the data token #FE already clocks the first data byte. The commands,
// chip selects (#77: SD1 = %00000001, SD2 = %00001011, none = %00000011)
// and the DMA setup are those of the SD driver of WC (DSDZC).

#include <string.h>
#include "sdr.h"

__sfr __at 0x57 SDR_DATA;
__sfr __at 0x77 SDR_CONF;
__sfr __banked __at 0x1DAF SDR_DMADL;
__sfr __banked __at 0x1EAF SDR_DMADH;
__sfr __banked __at 0x1FAF SDR_DMADX;
__sfr __banked __at 0x26AF SDR_DMALEN;
__sfr __banked __at 0x27AF SDR_DMACTR;
__sfr __banked __at 0x28AF SDR_DMANUM;
__sfr __banked __at 0x2CAF SDR_DMANUMH;

#define CONF_NONE 0x03
#define CONF_SD1  0x01
#define CONF_SD2  0x0B

#define WC_API    0x6006
#define WC_LOAD512 0x30
#define WC_GIPAGPL 0x32
#define WC_CHTOSEP 0x34

#ifndef SDR_STATE_ADDR
SDR_STATE sdr;
#endif

static u32 fat_lba, data_lba, max_cl;
static u32 fat_cached;
static u8 *fat_buf;
static u8 *io_dst;

// --- WC API ------------------------------------------------------------

// The calls keep IX and IY: IX is the frame pointer of the C code.
static void wc_gipagpl() __naked
{
  __asm
    push ix
    push iy
    ld a, #WC_GIPAGPL
    call WC_API
    pop iy
    pop ix
    ret
  __endasm;
}

// One sector of the WC stream into io_dst.
static void wc_load1() __naked
{
  __asm
    push ix
    push iy
    ld hl, (_io_dst)
    ld b, #1
    ld a, #WC_LOAD512
    call WC_API
    pop iy
    pop ix
    ret
  __endasm;
}

// LBA list of the first cluster into io_dst (up to 128 entries): CHTOSEP
// writes a whole cluster before it checks the end, so the end is io_dst + 4.
static void wc_chtosep() __naked
{
  __asm
    ld de, (_io_dst)
    ld h, d
    ld l, e
    inc hl
    inc hl
    inc hl
    inc hl
    ld b, h
    ld c, l
    push ix
    push iy
    ld a, #WC_CHTOSEP
    call WC_API
    pop iy
    pop ix
    ret
  __endasm;
}

// --- SPI ---------------------------------------------------------------

static void deselect()
{
  SDR_CONF = CONF_NONE;
  SDR_DATA = 0xFF;
}

static u8 wait_ready()
{
  u16 n = 0;
  do
  {
    if (SDR_DATA == 0xFF) return 1;
  } while (--n);
  return 0;
}

// Send a command, return R1 (#FF: no answer).
static u8 cmd(u8 c, u32 arg)
{
  u8 n, r;
  deselect();
  SDR_CONF = sdr.cs;
  SDR_DATA = 0xFF;
  wait_ready();
  SDR_DATA = 0x40 | c;
  SDR_DATA = arg >> 24;
  SDR_DATA = arg >> 16;
  SDR_DATA = arg >> 8;
  SDR_DATA = arg;
  SDR_DATA = 0xFF;
  for (n = 0; n < 10; n++)
  {
    r = SDR_DATA;
    if (!(r & 0x80)) return r;
  }
  return 0xFF;
}

static u32 addr(u32 lba)
{
  return sdr.hc ? lba : lba << 9;
}

// Wait for the data token; the read that returns it clocks data byte 0.
static u8 wait_token()
{
  u16 n = 0;
  u8 r;
  do
  {
    r = SDR_DATA;
    if (r != 0xFF) return r == 0xFE;
  } while (--n);
  return 0;
}

// 512 bytes into io_dst by INIR (port #0057: B counts, the low byte decodes).
static void read_block() __naked
{
  __asm
    ld hl, (_io_dst)
    ld bc, #0x0057
    inir
    inir
    in a, (c)        ; CRC
    in a, (c)
    ret
  __endasm;
}

// Stop a multiple block read: CMD12, a stuff byte, R1, busy.
static void stop_read()
{
  u8 n;
  SDR_DATA = 0x40 | 12;
  SDR_DATA = 0;
  SDR_DATA = 0;
  SDR_DATA = 0;
  SDR_DATA = 0;
  SDR_DATA = 0xFF;
  (void)SDR_DATA;
  for (n = 0; n < 10; n++)
    if (!(SDR_DATA & 0x80)) break;
  wait_ready();
}

bool sdr_read_lba(u32 lba, u8 *dst)
{
  bool ok = false;
  if (!cmd(17, addr(lba)) && wait_token())
  {
    io_dst = dst;
    read_block();
    ok = true;
  }
  deselect();
  return ok;
}

// --- probe -------------------------------------------------------------

// Card cs answers CMD58 as initialised; reads lba equal to sample.
static bool probe(u8 cs, u32 lba, u8 *sample, u8 *buf)
{
  u8 ocr;
  sdr.cs = cs;
  if (cmd(58, 0))
  {
    deselect();
    return false;
  }
  ocr = SDR_DATA;
  (void)SDR_DATA;
  (void)SDR_DATA;
  (void)SDR_DATA;
  deselect();
  if (!(ocr & 0x80)) return false;    // power up not finished
  sdr.hc = (ocr & 0x40) ? 1 : 0;
  return sdr_read_lba(lba, buf) && !memcmp(buf, sample, 512);
}

// --- volume ------------------------------------------------------------

static u16 rd16(u8 *p) { return p[0] | ((u16)p[1] << 8); }
static u32 rd32(u8 *p) { return rd16(p) | ((u32)rd16(p + 2) << 16); }

// BPB of a FAT32 volume at vol in buf: sets fat_lba, data_lba, sdr.spc,
// max_cl; true when it is one and lba is in its data area.
static bool bpb(u32 vol, u8 *buf, u32 lba)
{
  u8 spc = buf[13];
  u16 rsv = rd16(buf + 14);
  u8 nfat = buf[16];
  u32 tot = rd32(buf + 32);
  u32 fsz = rd32(buf + 36);
  if (rd16(buf + 11) != 512 || !spc || (spc & (spc - 1)) || !rsv || !nfat ||
      rd16(buf + 17) || rd16(buf + 22) || !fsz || buf[510] != 0x55 || buf[511] != 0xAA)
    return false;
  fat_lba = vol + rsv;
  data_lba = fat_lba + fsz * nfat;
  if (lba < data_lba || lba >= vol + tot) return false;
  sdr.spc = spc;
  for (sdr.shift = 0; (1 << sdr.shift) != spc; sdr.shift++);
  max_cl = ((vol + tot - data_lba) >> sdr.shift) + 1;
  if (max_cl > (fsz << 7) - 1) max_cl = (fsz << 7) - 1;
  return true;
}

// The FAT32 volume of lba: the card itself (no MBR) or a primary partition.
static bool volume(u8 *buf, u32 lba)
{
  u8 i;
  u8 *e;
  u32 start, size;
  if (!sdr_read_lba(0, buf)) return false;
  if (bpb(0, buf, lba)) return true;
  if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
  for (i = 0; i < 4; i++)
  {
    e = buf + 0x1BE + i * 16;
    start = rd32(e + 8);
    size = rd32(e + 12);
    if (!e[4] || lba < start || lba - start >= size) continue;
    if (!sdr_read_lba(start, buf)) return false;
    return bpb(start, buf, lba);
  }
  return false;
}

// --- chain -------------------------------------------------------------

// FAT entry of cluster cl, 0 on a read error.
static u32 fat_next(u32 cl)
{
  u32 sec = fat_lba + (cl >> 7);
  if (sec != fat_cached)
  {
    if (!sdr_read_lba(sec, fat_buf)) return 0;
    fat_cached = sec;
  }
  return rd32(fat_buf + ((u16)(cl & 127) << 2)) & 0x0FFFFFFFUL;
}

// Contiguous links in the cached FAT sector: from fr_ptr, at most fr_n
// entries, each equal to its own cluster + 1 (fr_val for the first, masked
// to 28 bits). The count goes to fr_cnt.
static u8 *fr_ptr;
static u8 fr_n, fr_cnt;
static u32 fr_val;

static void fat_run() __naked
{
  __asm
    ld hl, (_fr_ptr)
    ld de, (_fr_val)
    ld c, #0
    ld a, (_fr_n)
    or a
    jr z, 00002$
    ld b, a
00001$:
    ld a, (hl)
    cp e
    jr nz, 00002$
    inc hl
    ld a, (hl)
    cp d
    jr nz, 00002$
    inc hl
    ld a, (_fr_val + 2)
    cp (hl)
    jr nz, 00002$
    inc hl
    ld a, (_fr_val + 3)
    xor (hl)
    and #0x0F
    jr nz, 00002$
    inc hl
    inc c
    inc de
    ld a, d
    or e
    jr nz, 00003$
    push hl
    ld hl, (_fr_val + 2)
    inc hl
    ld (_fr_val + 2), hl
    pop hl
00003$:
    djnz 00001$
00002$:
    ld a, c
    ld (_fr_cnt), a
    ret
  __endasm;
}

static u8 walk(u32 cl)
{
  u32 fcl = 0, prev = 0, left;
  u8 idx;
  sdr.next = 0;
  fat_cached = 0xFFFFFFFFUL;
  for (;;)
  {
    if (cl < 2 || cl > max_cl) return SDR_E_FAT;
    if (!fcl || cl != prev + 1)
    {
      if (sdr.next == SDR_MAX_EXT) return SDR_E_FRAG;
      sdr.ext[sdr.next].fcl = fcl;
      sdr.ext[sdr.next].lba = data_lba + ((cl - 2) << sdr.shift);
      sdr.next++;
    }
    left = sdr.ncl - 1 - fcl;        // links still to follow
    if (!left) break;
    if (!fat_next(cl)) return SDR_E_IO;  // caches the sector of cl
    idx = cl & 127;
    fr_ptr = fat_buf + ((u16)idx << 2);
    fr_n = 128 - idx;
    if (left < fr_n) fr_n = left;
    fr_val = cl + 1;
    fat_run();
    if (fr_cnt)
    {
      // fr_cnt links, each to the next cluster: the run goes on.
      cl += fr_cnt;
      fcl += fr_cnt;
      prev = cl - 1;
    }
    else
    {
      prev = cl;
      cl = fat_next(cl);
      fcl++;
    }
  }
  return SDR_OK;
}

static u16 ext_of(u32 fcl);

u32 sdr_lba(u32 fsec)
{
  u32 fcl = fsec >> sdr.shift;
  u16 i;
  if (fcl >= sdr.ncl) return 0;
  i = ext_of(fcl);
  return sdr.ext[i].lba + ((fcl - sdr.ext[i].fcl) << sdr.shift) + (fsec & (sdr.spc - 1));
}

u8 sdr_open(u8 *buf, u32 size)
{
  u8 *sample = buf, *list = buf + 512, *sec = buf + 1024;
  u32 lba, cl;
  u8 i, r;
  sdr.next = 0;
  if (!size) return SDR_E_EMPTY;
  // The first sector through WC, then the LBAs of the first cluster.
  wc_gipagpl();
  io_dst = sample;
  wc_load1();
  wc_gipagpl();
  memset(list, 0, 512);
  io_dst = list;
  wc_chtosep();
  wc_gipagpl();
  lba = rd32(list);
  if (!lba) return SDR_E_CHAIN;
  r = SDR_E_CARD;
  if (probe(CONF_SD1, lba, sample, sec) || probe(CONF_SD2, lba, sample, sec))
  {
    r = SDR_E_VOLUME;
    if (volume(sec, lba))
    {
      r = SDR_E_ALIGN;
      if (!((lba - data_lba) & (sdr.spc - 1)))
      {
        cl = ((lba - data_lba) >> sdr.shift) + 2;
        sdr.ncl = ((size - 1) >> (9 + sdr.shift)) + 1;
        fat_buf = sec;
        r = walk(cl);
        // CHTOSEP gave the first cluster: the same sectors, no more.
        for (i = 0; r == SDR_OK && i < 128; i++)
          if (rd32(list + (i << 2)) != (i < sdr.spc ? lba + i : 0))
            r = SDR_E_VERIFY;
      }
    }
  }
  deselect();
  if (r != SDR_OK) sdr.next = 0;
  return r;
}

// --- DMA read ------------------------------------------------------------

// rb_n blocks of an open CMD18 into page rb_page from rb_offs: for each the
// data token, DMA SPI -> RAM of 512 bytes (DMACtrl #42, 256 bursts of a word,
// as DSDZC), the CRC. rb_ok 0: no token or an error token.
static u8 rb_n, rb_page, rb_ok;
static u16 rb_offs;

static void read_blocks() __naked
{
  __asm
00001$:
    ld hl, #0
00002$:
    in a, (#0x57)
    cp #0xFF
    jr nz, 00003$
    dec hl
    ld a, h
    or l
    jr nz, 00002$
    jr 00009$
00003$:
    cp #0xFE
    jr nz, 00009$
    ld c, #0xAF
    ld b, #0x1F
    ld a, (_rb_page)
    out (c), a
    ld hl, (_rb_offs)
    ld b, #0x1E
    out (c), h
    ld b, #0x1D
    out (c), l
    xor a
    ld b, #0x26
    out (c), a
    ld b, #0x2C
    out (c), a
    ld b, #0x28
    dec a
    out (c), a
    ld b, #0x27
    ld a, #0x42
    out (c), a
00004$:
    in a, (c)
    rlca
    jr c, 00004$
    in a, (#0x57)
    in a, (#0x57)
    inc h
    inc h
    ld (_rb_offs), hl
    ld hl, #_rb_n
    dec (hl)
    jr nz, 00001$
    ld a, #1
    ld (_rb_ok), a
    ret
00009$:
    xor a
    ld (_rb_ok), a
    ret
  __endasm;
}

// Index of the extent of file cluster fcl (< sdr.ncl).
static u16 ext_of(u32 fcl)
{
  u16 i;
  for (i = sdr.next - 1; sdr.ext[i].fcl > fcl; i--);
  return i;
}

bool sdr_read(u32 fsec, u8 count, u8 page, u16 offs)
{
  u32 fcl, lba, left;
  u16 i;
  u8 run;
  bool ok = true;
  while (count && ok)
  {
    fcl = fsec >> sdr.shift;
    if (fcl >= sdr.ncl) return false;
    i = ext_of(fcl);
    lba = sdr.ext[i].lba + ((fcl - sdr.ext[i].fcl) << sdr.shift) + (fsec & (sdr.spc - 1));
    // sectors up to the end of the extent: one CMD18 for them
    left = ((i + 1 < sdr.next ? sdr.ext[i + 1].fcl : sdr.ncl) << sdr.shift) - fsec;
    run = left < count ? left : count;
    if (cmd(18, addr(lba)))
      ok = false;
    else
    {
      rb_n = run;
      rb_page = page;
      rb_offs = offs;
      read_blocks();
      ok = rb_ok;
      offs = rb_offs;
      stop_read();
    }
    deselect();
    fsec += run;
    count -= run;
  }
  return ok;
}
