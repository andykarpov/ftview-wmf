#pragma once
// SD reader of the plugin: reads the file that WC started the plugin with
// straight from the card (Z-Controller ports #77 / #57, DMA SPI -> RAM),
// at any position. WC is used only through its public API: GIPAGPL and
// LOAD512 (the first sector of the file, to find the card) and CHTOSEP
// (API 52, the LBA of the first cluster). The chain of clusters is walked
// in the FAT of the volume by the reader itself.

#include <defs.h>

enum
{
  SDR_OK,
  SDR_E_EMPTY,    // file size 0
  SDR_E_SAMPLE,   // LOAD512 of the first sector failed
  SDR_E_CHAIN,    // CHTOSEP returned no sectors
  SDR_E_CARD,     // no card returns the first sector of the file
  SDR_E_VOLUME,   // no FAT32 volume holds the file
  SDR_E_ALIGN,    // the first LBA is not the start of a cluster
  SDR_E_FAT,      // bad link in the chain (free, out of range, early end)
  SDR_E_FRAG,     // more extents than SDR_MAX_EXT
  SDR_E_VERIFY,   // the chain disagrees with CHTOSEP
  SDR_E_IO,       // read of a sector failed
};

// Extents: runs of consecutive clusters. fcl is the index in the file of
// the first cluster of the run, lba the sector of that cluster.
#define SDR_MAX_EXT 30

typedef struct
{
  u32 fcl;
  u32 lba;
} SDR_EXT;

// The whole state of the reader, 256 bytes. The plugin keeps it in the tail
// of its buffer page 1 (SDR_STATE_ADDR, #FE00, valid while that page is in
// #C000): the DATA of a code bank is too small. Elsewhere it is a global.
typedef struct
{
  u8 cs;                   // #77 value that selects the card (1 SD1, #0B SD2)
  u8 hc;                   // 1: block addressing (SDHC / SDXC)
  u8 spc;                  // sectors per cluster
  u8 shift;                // log2(spc)
  u16 next;                // extents in ext
  u32 ncl;                 // clusters of the file
  u8 res[6];
  SDR_EXT ext[SDR_MAX_EXT];
} SDR_STATE;

#ifdef SDR_STATE_ADDR
#define sdr (*(SDR_STATE*)SDR_STATE_ADDR)
#else
extern SDR_STATE sdr;
#endif

// buf: at least 1536 bytes of scratch. Leaves the WC stream at the start of
// the file (GIPAGPL), all chip selects off.
u8 sdr_open(u8 *buf, u32 size);
// LBA of sector fsec of the file (0..), 0 when past its chain.
u32 sdr_lba(u32 fsec);
// Read count (1..32) sectors from sector fsec of the file into the RAM page
// page at offset offs (0..#3FFF, count * 512 must fit in the page), by DMA.
// Crosses extents by itself. false: a card error or past the chain.
bool sdr_read(u32 fsec, u8 count, u8 page, u16 offs);
// Read one sector at LBA lba into dst (Z80 address) without DMA.
bool sdr_read_lba(u32 lba, u8 *dst);
