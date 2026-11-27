# AVI sound through the ESP32 (ZiFi32)

Decided 2026-11-27 with the user.

## Routing

The sound of a frame-by-frame AVI (MJPEG video) goes, in this order, to:

1. the ESP32: the audio stream player of the ZiFi32 firmware (commands 0x90..0x99,
   `pentevo/esp32/doc/esp32-audio.md`; in the Korochinskiy firmware the boards use).
   Every format it decodes: WAV family (PCM 8 / 16, IMA ADPCM, A-law, u-law) behind a
   WAV header made from the `strf` of the AVI, MP3 (chunks as they are), AAC (raw AAC
   frames wrapped in ADTS headers from the AudioSpecificConfig of `strf`).
2. General Sound: PCM 8 bit up to 37.5 kHz (the driver as now).
3. FT812 audio: PCM 8 bit (as now; its output goes to the ADC of the board).
4. Anything else: `CMD_PLAYVIDEO` (no seek, no HUD), as now.

No option in wc.ini: the plugin probes the ESP (`AUDIO_GET_STATE` answers READY) and GS.

## Design

- The ESP shares the SPI (#57) with the FT812 and the SD card: it is talked to only
  between transfers, never during the DMA of a block into the media FIFO.
- The ESP code lives in the I/O bank (page 10, ~9 KB free; the video bank has ~280
  bytes). The video bank stages the audio bytes of a block (pcm_tee) by a far_call;
  the I/O bank keeps a staging ring and pushes it (`AUDIO_PUSH`, multiples of 4) when
  the ESP has room. Parameters: a block at the tail of the page in #C000 (#FFC0..,
  free in every buffer page).
- Clock: samples played = base + `PLAYED` of the ESP (source frames since START).
  base = the time of the first audio byte pushed after a start or a seek.
- Seek: a new `AUDIO_START` (with a new WAV header for the WAV family) and the audio
  from the seek chunk on; base from the audio position of that chunk (bytes for
  CBR formats / blocks for ADPCM, chunk count for VBR MP3 / AAC: the index keeps
  both).
- End: `AUDIO_END` after the last audio chunk, ENDED ends the sound.

## Phases (one PR each)

1. ESP client in the I/O bank (detect, START, PUSH with the M2S / W_END handling of
   espplay-wmf, GET_STATE, PAUSE, STOP, END) + test model (from espplay-wmf espsim.py).
2. Player: route PCM 8 / 16 and IMA ADPCM to the ESP (WAV header), clock from PLAYED,
   pause, seek by restart, end. Board: 3P.AVI (PCM 8), 3.avi (ADPCM), DT_intro.avi.
3. MP3 in AVI.
4. AAC in AVI (ADTS).
