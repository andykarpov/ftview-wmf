#pragma once
// The audio stream player of the ESP32 (ZiFi32: commands 0x90..0x99,
// pentevo/esp32/doc/esp32-audio.md of tslabs/zx-evo) for the sound of an AVI.
// The client lives in the I/O bank (esp.c); the other banks call it by
// far_call(IO_PAGE, ESP_FAR) with ESP_SHARED at the tail of the page in #C000.

#include <defs.h>

// AUDIO_FMT_*
#define ESP_FMT_WAV 1
#define ESP_FMT_MP3 2
#define ESP_FMT_AAC 5

// AUDIO_ST_*
#define ESP_A_IDLE 0
#define ESP_A_WAIT 1
#define ESP_A_PLAY 2
#define ESP_A_DRAIN 3
#define ESP_A_ENDED 4
#define ESP_A_ERROR 5

// Operations (ESP_SHARED.op), all in the I/O bank.
#define ESP_DETECT 0x41   // ok: an ESP with the player answers
#define ESP_START 0x42    // a new stream of fmt; for WAV the header from wfx
#define ESP_STAGE 0x43    // len bytes at ptr (window #C000) into the ring; ok 0: no room
#define ESP_POLL 0x44     // push what the ring holds while the ESP has room, then the state
#define ESP_PAUSE 0x45    // param 1 pause / 0 go on
#define ESP_STOP 0x46     // stop, the ring emptied
#define ESP_END 0x47      // no more data: push the rest, then AUDIO_END

// The staging ring of the I/O bank: what the video bank took out of a block
// waits here until the ESP has room (FREE).
#define ESP_RING 0xA800
#define ESP_RING_SIZE 0x1400

typedef struct
{
  u8 op, ok;
  u8 fmt, param;
  u16 ptr, len;
  // ESP_POLL / ESP_END: the state after it
  u8 state;          // AUDIO_ST_*
  u8 err;            // the status of a refused command, 0 = none
  u16 staged;        // bytes in the ring
  u32 played;        // source frames played since START
  u32 rate;
  // ESP_START with fmt WAV: WAVEFORMATEX of the strf of the AVI and the
  // extra bytes after it (IMA ADPCM: wSamplesPerBlock)
  u8 wfx[20];
} ESP_SHARED;

// #FFC0..#FFEF of the page in #C000: free in every buffer page (page 1:
// after AVI_FAR #FF00..#FF58, before IO_FAR #FFF0).
#define ESP_FAR ((ESP_SHARED*)0xFFC0)

#ifdef FTVIEW_IO_BANK
void esp_entry(ESP_SHARED *p);
#endif
