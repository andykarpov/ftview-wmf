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
  p->offs = m->h_visible;
  p->fsec = m->v_visible;
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

void far_entry(IO_SHARED *p)
{
  if (p->op == IO_OPEN) p->ok = sdr_open((u8*)0xC000, p->size) == SDR_OK;
  else if (p->op == IO_READ) p->ok = sdr_read(p->fsec, p->n, p->page, p->offs);
  else if (p->op == IO_FT_INIT) ft_init_mode(p);
  else if (p->op >= ESP_DETECT) esp_entry((ESP_SHARED*)p);
  else p->ok = 0;
}
