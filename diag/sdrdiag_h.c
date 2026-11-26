
#include <defs.h>
#include <wc_h.h>

static const WC_HDR hdr =
{
  /* +00 -reserved-          */   "",
  /* +16 Magic string        */   {'W', 'i', 'l', 'd', 'C', 'o', 'm', 'm', 'a', 'n', 'd', 'e', 'r', 'M', 'D', 'L'},
  /* +32 Format version      */   7,
  /* +33 -reserved-          */   "",
  /* +34 Number of pages     */   2,  // the code and one buffer page
  /* +35 Page at 0x8000      */   0,
  /* +36 Page definitions    */   {
  /*  +0 Page, +1 NBlocks    */     {0, N_BLK0}, {1, 0}
  /*                         */   },
  /* +48 -reserved-          */   "",
  /* +64 Extensions          */   {
  /*                         */     {'S', 'D', 'T'},
  /*                         */     {'M', 'P', '4'},
  /*                         */     {'M', 'K', 'V'},
  /*                         */   },
  /* +160 0x00               */   "",
  /* +161 Max file size      */   0xFFFFFFFF,
  /* +165 Plugin name        */   "SD reader test",
  /* +197 Starting condition */   SC_EXT
};
