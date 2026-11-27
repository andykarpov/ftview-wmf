// The ESP32 audio client of the I/O bank (esp.h). The register protocol of
// ZiFi32 over the SPI of TS-Conf (#77 = #13 selects the ESP, #57 data), as
// src/esp.asm of espplay-wmf, which was checked on the board: WR_REGS,
// RD_REGS, WR_DATA + W_END for the data of AUDIO_PUSH (padded to 4 bytes), the
// late M2S and the lost W_END of the receiver task of the ESP.
#include <defs.h>
#include <string.h>
#include "esp.h"

__sfr __at 0x57 ESP_SPI_DATA;
__sfr __at 0x77 ESP_SPI_CTRL;

#define CS_ON 0x13
#define CS_OFF 0x03
#define SPI_WR_REGS 0x01
#define SPI_RD_REGS 0x02
#define SPI_WR_DATA 0x03
#define SPI_W_END 0x07

#define R_COMMAND 0x00
#define R_STATUS 0x01
#define R_PARAM 0x02
#define R_SIZE 0x03
#define R_STATE 0x10

#define ST_IDLE 0x00
#define ST_READY 0x01
#define ST_BUSY 0x02
#define ST_M2S 0x03
#define TIMEOUT 0xFF       // esp_wait: no answer in time

#define C_START 0x90
#define C_PUSH 0x91
#define C_END 0x92
#define C_PAUSE 0x94
#define C_STOP 0x96
#define C_STATE 0x97

#define M2S_WAITS 4
#define WEND_TRIES 3
#define PUSH_MAX 4096      // bytes of one PUSH (the ESP takes up to 16384)
// A PUSH has a fixed cost (about 2 ms of SPI frames and waits for the ESP,
// measured on the board) besides its bytes: the ring goes out in batches,
// once PUSH_MIN is in it or PUSH_AGE WC ticks after the last PUSH (by age,
// not only by size: the audio of the stream is only about 0.3 s ahead of
// the play, and a size alone left the ESP without data).
#define PUSH_MIN 4096
#define PUSH_AGE 3
#define TMN_ (*(volatile u8*)0x6009)
u8 push_tick;

// The state block #10..#27: state, fmt, channels, flags, FREE, RATE, PLAYED,
// TOTAL, BUFFERED.
typedef struct
{
  u8 state, fmt, channels, flags;
  u32 free, rate, played, total, buffered;
  u16 underruns;
} ESP_STATE;

ESP_STATE esp_st;
u16 ring_head, ring_tail, ring_count;
u8 esp_end_sent, esp_end_wanted;
// PLAYED -> bytes: align bytes hold spb frames (from ESP_WFX at START).
u16 esp_align, esp_spb;
__at (ESP_RING) u8 esp_ring[ESP_RING_SIZE];
#ifdef FTVIEW_ESP_DEBUG
#define TMN (*(volatile u8*)0x6009)
u8 d_win, d_push, d_all, d_pushes, d_r[3];
#endif

static void wr_regs(u8 reg, const u8 *p, u8 n)
{
  ESP_SPI_CTRL = CS_ON;
  ESP_SPI_DATA = SPI_WR_REGS;
  ESP_SPI_DATA = reg;
  (void)ESP_SPI_DATA;
  while (n--) ESP_SPI_DATA = *p++;
  ESP_SPI_CTRL = CS_OFF;
}

static void rd_regs(u8 reg, u8 *p, u8 n)
{
  ESP_SPI_CTRL = CS_ON;
  ESP_SPI_DATA = SPI_RD_REGS;
  ESP_SPI_DATA = reg;
  (void)ESP_SPI_DATA;
  (void)ESP_SPI_DATA;
  while (n--) *p++ = ESP_SPI_DATA;
  ESP_SPI_CTRL = CS_OFF;
}

static u8 rd8(u8 reg)
{
  u8 v;
  rd_regs(reg, &v, 1);
  return v;
}

// COMMAND = c, STATUS = IDLE in one write.
static void cmd(u8 c)
{
  u8 b[2];
  b[0] = c; b[1] = ST_IDLE;
  wr_regs(R_COMMAND, b, 2);
}

// The status once it is no longer IDLE / BUSY (nor M2S with m2s_busy: after a
// W_END); TIMEOUT after about a second.
static u8 wait(bool m2s_busy)
{
  u16 n = 0;
  do
  {
    u8 s = rd8(R_STATUS);
    if (s != ST_IDLE && s != ST_BUSY && (s != ST_M2S || !m2s_busy)) return s;
  } while (--n);
  return TIMEOUT;
}

static u8 run(u8 c)
{
  cmd(c);
  return wait(false);
}

static u8 run_p(u8 c, u8 param)
{
  wr_regs(R_PARAM, &param, 1);
  return run(c);
}

// n bytes at p out of #57 by OTIR (a C loop took about 2.6 us a byte).
static void spi_out(const u8 *p, u16 n) __naked
{
  p; n;
  __asm
    ld c, #0x57
    ld a, e
    or a
    jr z, 00001$
    ld b, a
    otir
00001$:
    ld a, d
    or a
    ret z
    ld b, #0
00002$:
    otir
    dec d
    jr nz, 00002$
    ret
  __endasm;
}

static void rd_state()
{
  rd_regs(R_STATE, (u8*)&esp_st, sizeof(esp_st));
}

// AUDIO_PUSH of n bytes at p. false: refused or lost (the caller keeps them).
static bool push_(const u8 *p, u16 n);
static bool push(const u8 *p, u16 n)
{
#ifdef FTVIEW_ESP_DEBUG
  u8 t0 = TMN;
  bool r = push_(p, n);
  d_push += (u8)(TMN - t0);
  ++d_pushes;
  return r;
#else
  return push_(p, n);
#endif
}

static bool push_(const u8 *p, u16 n)
{
  u8 s, tries, pad;
  u16 k;
  u32 size = n;
  wr_regs(R_SIZE, (u8*)&size, 4);
  cmd(C_PUSH);
  // M2S may come late (the receiver task arms its DMA after more than one
  // wait, ~0.6 s was seen): up to M2S_WAITS of them.
  for (k = M2S_WAITS; k; --k)
    if ((s = wait(false)) != TIMEOUT) break;
  if (s != TIMEOUT && s != ST_M2S) return false;     // refused (size, state)
  // No M2S in time: the ESP may arm a moment later and then wait for this
  // data for ever: sent anyway, but a later M2S may be the next PUSH's, so
  // only one W_END then.
  tries = s == TIMEOUT ? 1 : WEND_TRIES;
  ESP_SPI_CTRL = CS_ON;
  ESP_SPI_DATA = SPI_WR_DATA;
  (void)ESP_SPI_DATA;
  (void)ESP_SPI_DATA;
  spi_out(p, n);
  // The DMA of the ESP takes words: zeros up to a multiple of 4.
  for (pad = -(u8)n & 3; pad; --pad) ESP_SPI_DATA = 0;
  ESP_SPI_CTRL = CS_OFF;
  for (;;)
  {
    ESP_SPI_CTRL = CS_ON;
    ESP_SPI_DATA = SPI_W_END;
    (void)ESP_SPI_DATA;
    ESP_SPI_CTRL = CS_OFF;
    s = wait(true);
    if (s == TIMEOUT) s = rd8(R_STATUS);
    // Still M2S: the ESP has the data but missed the W_END: again.
    if (s != ST_M2S || !--tries) break;
  }
  rd_state();
  return s == ST_READY;
}

// The ring to the ESP while its FREE allows, at most PUSH_MAX a PUSH; then
// AUDIO_END once wanted and the ring is empty.
static void drain(bool force)
{
  if ((u8)(TMN_ - push_tick) >= PUSH_AGE) force = true;
  while (ring_count >= PUSH_MIN || ((force || esp_end_wanted) && ring_count))
  {
    push_tick = TMN_;
    u16 n = ring_count;
    if (n > ESP_RING_SIZE - ring_tail) n = ESP_RING_SIZE - ring_tail;
    if (n > PUSH_MAX) n = PUSH_MAX;
    if (!esp_st.free) break;
    if (esp_st.free < n) n = esp_st.free;
    if (!push(esp_ring + ring_tail, n)) break;
    ring_tail += n;
    if (ring_tail == ESP_RING_SIZE) ring_tail = 0;
    ring_count -= n;
  }
  if (esp_end_wanted && !ring_count && !esp_end_sent)
  {
    esp_end_sent = 1;
    run(C_END);
  }
}

// A WAV header for the stream: RIFF, fmt (the WAVEFORMATEX of the AVI with
// its extra bytes) and a data chunk of unknown length (the stream ends with
// AUDIO_END).
static void wav_header()
{
  const u8 *wfx = ESP_WFX;
  u8 *h = esp_ring + ring_head;
  u8 extra = wfx[16];       // cbSize
  u8 fmt_len;
  if (extra > 2) extra = 2;
  fmt_len = 18 + extra;
  memcpy(h, "RIFF\xF8\xFF\xFF\x7FWAVEfmt ", 16);
  h[16] = fmt_len; h[17] = h[18] = h[19] = 0;
  memcpy(h + 20, wfx, fmt_len);
  h[36] = extra;            // cbSize as kept
  h[37] = 0;
  memcpy(h + 20 + fmt_len, "data\xD0\xFF\xFF\x7F", 8);
  ring_head += 28 + fmt_len;
  ring_count += 28 + fmt_len;
}

void esp_entry(ESP_SHARED *p)
{
  u8 s = 0;
#ifdef FTVIEW_ESP_DEBUG
  u8 t0 = TMN;
#endif
  p->ok = 1;
  switch (p->op)
  {
    case ESP_DETECT:
      s = run(C_STATE);
      p->ok = s == ST_READY;
      if (p->ok) rd_state();
      break;
    case ESP_START:
      ring_head = ring_tail = ring_count = 0;
      esp_end_sent = esp_end_wanted = 0;
    {
      // The format from the WAVEFORMATEX: MP3 (0x55) as it is, the rest of
      // the WAV family behind a WAV header. PLAYED -> bytes: a block of
      // nBlockAlign bytes holds wSamplesPerBlock frames (IMA ADPCM) or one;
      // MP3 (constant bitrate): nAvgBytesPerSec bytes a nSamplesPerSec frames.
      const u16 *w = (const u16*)ESP_WFX;
      p->fmt = w[0] == 0x55 ? ESP_FMT_MP3 : ESP_FMT_WAV;
      esp_align = w[0] == 0x55 ? w[4] : w[6] ? w[6] : 1;
      esp_spb = w[0] == 0x55 ? w[2] : w[0] == 0x11 && w[9] ? w[9] : 1;
      s = run_p(C_START, p->fmt);
      p->ok = s == ST_READY;
      // Paused until the player has its first frame (PAUSE 0): PAUSE is a
      // flag of the ESP in any state, START clears it.
      if (p->ok) s = run_p(C_PAUSE, 1);
      rd_state();
      if (p->ok && p->fmt == ESP_FMT_WAV) wav_header();
      break;
    }
    case ESP_STAGE:
    {
      u16 n = p->len, part;
      const u8 *src = (const u8*)p->ptr;
      // No room: what the ring holds goes out now, below PUSH_MIN too (the
      // caller stages before the DMA of the block: the SPI is free).
      if (n > ESP_RING_SIZE - ring_count)
      {
        if (run(C_STATE) == ST_READY) { rd_state(); drain(true); }
        if (n > ESP_RING_SIZE - ring_count) { p->ok = 0; break; }
      }
      ring_count += n;
      while (n)
      {
        part = ESP_RING_SIZE - ring_head;
        if (part > n) part = n;
        memcpy(esp_ring + ring_head, src, part);
        src += part; n -= part;
        ring_head += part;
        if (ring_head == ESP_RING_SIZE) ring_head = 0;
      }
      break;
    }
    case ESP_END:
      esp_end_wanted = 1;
      // fall through
    case ESP_POLL:
      s = run(C_STATE);
      if (s == ST_READY) { rd_state(); drain(false); }
      break;
    case ESP_PAUSE:
      s = run_p(C_PAUSE, p->param);
      break;
    case ESP_STOP:
      ring_head = ring_tail = ring_count = 0;
      s = run(C_STOP);
      break;
  }
  p->err = s == ST_READY || !s ? 0 : s;
  p->state = esp_st.state;
  p->staged = ring_count;
#ifdef FTVIEW_ESP_DEBUG
  d_all += (u8)(TMN - t0);
  if ((u8)(TMN - d_win) >= 50)
  {
    // underruns of the ESP (low byte), its input buffer in KB, PUSHes a second
    d_r[0] = esp_st.state; d_r[1] = esp_st.flags; d_r[2] = esp_st.buffered >> 10;
    d_push = d_all = d_pushes = 0;
    d_win = TMN;
  }
  p->d_push = d_r[0]; p->d_other = d_r[1]; p->d_pushes = d_r[2];
#endif
  // esp_spb is 0 before the first START (no GSINIT: globals start as 0).
  p->played = esp_spb ? (esp_st.played / esp_spb) * esp_align +
                        (esp_st.played % esp_spb) * esp_align / esp_spb : 0;
}
