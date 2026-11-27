// The I/O bank (page 10): the SD reader for the image and video banks, which
// have no room for it. Entered only by far_call; crt0 needs the entries of a
// bank, the gates and the entry frame at the same addresses as the others.
#include <defs.h>
#include <ft812.h>
#include <ft812lib.h>
#include <tslib.h>
#include "tsconf.h"
#include <wc_api.h>
#include "sdr.h"
#include "io_bank.h"
#include "esp.h"
#include "avi_shared.h"

__at (0xBFE0) u16 ret_sp;
__at (0xBFE2) u16 ret_ix;
__at (0xBFE4) u16 ret_iy;
__at (0xBFE6) u32 filesize;
__at (0xBFEA) u8 file_ext;
__at (0xBFEB) u8 call_type;
__at (0xBFEC) u8 gs_loaded;
__at (0xBFED) u8 ft_state;
__at (0xBFEE) u16 scr_w;
__at (0xBFF0) u16 scr_h;

void main() __naked
{
  __asm
    ret
  __endasm;
}

void main_dispatch()
{
}

// The screen of the mode set up here (ft_init_mode), for avi_view.
u16 io_w, io_h;
u8 io_half;

// The mode of the FT812: option n of -mode of wc.ini (0: no options) ->
// ft_modes of the SDK; 3 (800x600@60, 40 MHz) by default. 1024x768@59 of the
// original plugin (mode 7) runs the FT812 at 64 MHz from 8 MHz x 8.
static const u8 mode_of_option[11] = {3, 10, 13, 14, 0, 1, 2, 3, 5, 6, 7};

// The registers as ft_init of the SDK. The waits for ID / CPURESET are bounded:
// a wrong status of the VDAC2 must not keep WC in the loop of ft_init. ok: 0 no
// VDAC2 (nothing changed), 1 no answer after the reset, 2 ready; offs, fsec:
// the visible width and height.
static void ft_init_mode(IO_SHARED *p)
{
  u8 cmdl[12];
  u16 tries = 0xFFFF;
  u8 opt = p->n;
  const FT_MODE *m = &ft_modes[mode_of_option[opt <= 10 ? opt : 0]];
  p->ok = 0;
  // As the original plugin (main.c): TS_STATUS of tsconf.h is the port #00AF
  // read as an __sfr, so ts_rreg reads the register numbered by that byte.
  // The status itself has no 7 in bits 2..0 on the Karabas ("No VDAC2");
  // this check passes there, keep it as it was.
  if ((ts_rreg(TS_STATUS) & 7) != 7) return;
  p->ok = 1;               // the video of WC is changed from here on
  ft_cmd(FT_CMD_PWRDOWN);
  ft_cmd(FT_CMD_ACTIVE);
  ft_cmd(FT_CMD_SLEEP);
  ft_cmd(FT_CMD_CLKEXT);
  ft_cmdp(FT_CMD_CLKSEL, m->f_mul | 0x40);
  ft_cmd(FT_CMD_ACTIVE);
  ft_cmd(FT_CMD_RST_PULSE);
  while (ft_rreg8(FT_REG_ID) != FT_ID || ft_rreg16(FT_REG_CPURESET))
    if (!--tries || *(volatile u8*)_ABT) return;

  ft_wreg8(FT_REG_PCLK, 0);
  {
    u16 h = m->h_fporch + m->h_sync + m->h_bporch;
    u16 v = m->v_fporch + m->v_sync + m->v_bporch;
    ft_wreg16(FT_REG_HCYCLE, h + m->h_visible);
    ft_wreg16(FT_REG_HOFFSET, h);
    ft_wreg16(FT_REG_HSYNC0, m->h_fporch);
    ft_wreg16(FT_REG_HSYNC1, m->h_fporch + m->h_sync);
    ft_wreg16(FT_REG_VCYCLE, v + m->v_visible);
    ft_wreg16(FT_REG_VOFFSET, v - 1);
    ft_wreg16(FT_REG_VSYNC0, m->v_fporch - 1);
    ft_wreg16(FT_REG_VSYNC1, m->v_fporch + m->v_sync - 1);
  }
  p->offs = io_w = m->h_visible;
  p->fsec = io_h = m->v_visible;
  io_half = io_w == 640 && io_h == 1024;   // mode 10: pixels twice as wide
  ft_wreg16(FT_REG_HSIZE, m->h_visible);
  ft_wreg16(FT_REG_VSIZE, m->v_visible);
  // The system clock of the mode (8 MHz x f_mul): the chip times the audio,
  // the video of PLAYVIDEO and its timers by this value, not by the real
  // clock; with the default 60 MHz the video ran 1.5 times slower at 40 MHz.
  ft_wreg32(FT_REG_FREQUENCY, (u32)m->f_mul * 8000000UL);
  ft_wreg8(FT_REG_SWIZZLE, 0);
  ft_wreg8(FT_REG_PCLK_POL, 0);
  ft_wreg8(FT_REG_CSPREAD, 0);
  ft_ccmd_start(cmdl);
  ft_ClearColorRGB(0, 0, 0);
  ft_Clear(1, 1, 1);
  ft_Display();
  ft_write_dl(cmdl, 3);
  ft_wreg8(FT_REG_DLSWAP, FT_DLSWAP_FRAME);
  ft_wreg8(FT_REG_ADAPTIVE_FRAMERATE, 0);
  ft_wreg32(FT_REG_GPIOX_DIR, 0x8000UL);
  ft_wreg32(FT_REG_GPIOX, 0x9200UL);
  ft_wreg8(FT_REG_PCLK, m->f_div);
  // Плагин использует таймер WC, отдельные прерывания FT812 ему не нужны.
  ft_wreg8(FT_REG_INT_EN, 0);
  p->ok = 2;
}

// The frame on the screen, after the probe (the sizes of avih).
// Кадр растягивается на экран с билинейной фильтрацией FT812: при нецелом
// растяжении (2,39:1, маленькие ролики) ступеньки мягче. По руководству FT81x
// такая точка рисуется за 1/4 такта вместо 1/16: кадр шириной до 1024 точек —
// до 256 тактов на строку при бюджете не меньше 2048.
static void avi_view(AVI_SHARED *a)
{
  // Сначала выбираем ограничивающую сторону в 32 битах. Промежуточная
  // высота узкого кадра могла переполнить u16; округлённая ширина 0 затем
  // приводила к делению на ноль при расчёте матрицы. Минимум — один пиксель.
  // Кадр вписывается во весь экран io_w×io_h (режим FT812), HUD рисуется
  // поверх низа (в 1024×768 кадр 4:3 512×384 — ровно вдвое на весь экран,
  // 16:9 512×288 — 1024×576 по центру, HUD тогда на нижнем чёрном поле).
  // Пиксели режима 10 (640×1024 на 1280×1024) вдвое шире: вписываем в
  // 1280 и сжимаем ширину обратно.
  u16 ew = io_w;
  if (io_half) ew <<= 1;
  if ((u32)a->height * ew > (u32)a->width * io_h)
  {
    a->view_h = io_h; a->view_w = (u32)a->width * io_h / a->height;
  }
  else
  {
    a->view_w = ew; a->view_h = (u32)a->height * ew / a->width;
  }
  if (io_half) a->view_w >>= 1;
  if (!a->view_w) a->view_w = 1;
  if (!a->view_h) a->view_h = 1;
  a->ta = (u32)a->width * 256 / a->view_w;
  a->te = (u32)a->height * 256 / a->view_h;
}

// The sound of the AVI of AVI_FAR (avi_probe of the image bank left the
// strh in AUD_STRH, the strf in ESP_WFX and the flags in AUD_FLAGS): sink,
// align, rate, total of a->...; a->ok 0 (CMD_PLAYVIDEO) when no sink takes it.
#define GS_INT_HZ 37500UL
static void avi_sound(AVI_SHARED *a)
{
  u8 f = *AUD_FLAGS;
  u32 a_scale = AUD_STRH[0], a_rate = AUD_STRH[1], a_len = AUD_STRH[2], a_ss = AUD_STRH[3];
  // Where the sound goes (avi.sink): PCM 8 mono to GS if the driver is
  // loaded and the rate fits its interrupt (the phase step in 16 bits): GS
  // takes the bytes during the DMA of the block. Else the ESP (ft_state bit
  // 6: found at the start) for every format it takes: its PUSHes cost the
  // Z80 a few ms each before the DMA (PCM 8 of 32 kHz through the ESP played
  // at 80 % speed on the board, IMA ADPCM at full speed). Else PCM 8 to the
  // FT812. The player counts the sound in bytes of the stream (for PCM 8 a
  // byte is a sample): rate = bytes a second from strh (the nAvgBytesPerSec
  // of strf lies in some files), total = bytes.
  a->sink = a->align = 0;
  a->pcm_rate = 0;
  if (f & 1)
  {
    u16 align = ((u16*)ESP_WFX)[6];
    u32 id = a_scale ? a_rate * a_ss / a_scale : 0;
    if ((f & 2) && a_rate <= 48000)
    {
      a->align = 1; a->pcm_rate = a_rate; a->pcm_total = a_len;
      a->sink = (f & 8) && a_rate <= GS_INT_HZ;
    }
#ifndef FTVIEW_NO_ESP
    // MP3: a frame a chunk (sample size 0); taken as constant bitrate: the
    // bytes a second of strf, the total from the length of strh in time, the
    // seek from any byte (the decoder finds the next frame).
    if (((u16*)ESP_WFX)[0] == 0x55 && a_rate)
    {
      a_ss = a_len * a_scale;                  // the length in 1 / a_rate s
      id = ((u32*)ESP_WFX)[2];                 // nAvgBytesPerSec
      a_len = (a_ss / a_rate) * id + (a_ss % a_rate) * id / a_rate;
      a_ss = align = 1;
    }
    if (!a->sink && (f & 0x10) && (f & 4) && a_ss == align && id && id <= 0xFFFF)
    { a->sink = 2; a->align = align; a->pcm_rate = id; a->pcm_total = a_len * a_ss; }
#endif
  }
  if (a->pcm_rate)
  {
    a->step = a->pcm_rate == GS_INT_HZ ? 0xFFFF : ((u32)a->pcm_rate << 16) / GS_INT_HZ;
    a->tick = a->pcm_rate / 40 + 1;
    a->unit = 14000000UL / 26 / a->pcm_rate;
  }
  // Два RGB565-кадра, слово результата и 128 КиБ media FIFO не пересекаются.
  // Кольцо FT812 лежит в RAM_G с #C0000; GS и ESP его не требуют.
  if ((f & 1) && (!a->align || !a->pcm_total || a->scale != 1 ||
                  !a->rate || a->rate > 1000 ||
                  a->bytes > (a->sink ? AVI_RESULT : PCM_BASE) / 2))
  { a->pcm_rate = 0; a->ok = 0; }
}

void far_entry(IO_SHARED *p)
{
  if (p->op == IO_OPEN) p->ok = sdr_open((u8*)0xC000, p->size) == SDR_OK;
  else if (p->op == IO_READ) p->ok = sdr_read(p->fsec, p->n, p->page, p->offs);
  else if (p->op == IO_FT_INIT) ft_init_mode(p);
  else if (p->op == IO_AVI)
  {
    avi_sound(AVI_FAR);
    if (AVI_FAR->ok == 1) avi_view(AVI_FAR);
  }
  else if (p->op >= ESP_DETECT) esp_entry((ESP_SHARED*)p);
  else p->ok = 0;
}
