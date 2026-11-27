
#include <defs.h>
#include <wc_h.h>

static const WC_HDR hdr =
{
  /* +00 -reserved-          */   "",
  /* +16 Magic string        */   {'W', 'i', 'l', 'd', 'C', 'o', 'm', 'm', 'a', 'n', 'd', 'e', 'r', 'M', 'D', 'L'},
  /* +32 Format version      */   7,
  /* +33 -reserved-          */   "",
  /* +34 Number of pages     */   11, // code banks 0 images, 9 video, 10 I/O; buffer pages 1..8
  /* +35 Page at 0x8000      */   0,
  /* +36 Page definitions    */   {
  /*  +0 Page, +1 NBlocks    */     {0, N_BLK0}, {9, N_BLK9}, {10, N_BLK10}, {1, 0}
  /*                         */   },
  /* +48 -reserved-          */   "",
  /* +64 Extensions          */   {
  /*                         */     {'J', 'P', 'G'},
  /*                         */     {'P', 'N', 'G'},
  /*                         */     {'D', 'L', 'S'},
  /*                         */     {'D', 'X', 'P'},
  /*                         */     {'A', 'V', 'I'}
  /*                         */   },
  /* +160 0x00               */   "",
  /* +161 Max file size      */   0xFFFFFFFF,
  /* +165 Plugin name        */   "FT812 Viewer",
  /* +197 Starting condition */   SC_EXT,   // только по расширению: это просмотрщик, в F10 ему делать нечего
  /* +198 -reserved-         */   {0},
  /* +260 Options of wc.ini  */   {
  // "FTVIEW.WMF -mode=N": N the FT812 mode of the SDK (ft_modes) whose pixel
  // clock the HDMI of the core takes (up to 64 MHz). WC matches a value by its
  // prefix: 10, 13, 14 before 1. Default (option 7) 3, 800x600@60 (40 MHz);
  // PRM_PL (API 83) returns the number of the option (mode_of_option).
  /*                         */     30, 7, 'm', 'o', 'd', 'e', 10, '1', '0', 0, '1', '3', 0, '1', '4', 0, '0', 0, '1', 0, '2', 0, '3', 0, '5', 0, '6', 0, '7', 0, 0
  /*                         */   }
};
