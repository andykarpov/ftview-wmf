// The I/O bank (page 10): the SD reader for the image and video banks, which
// have no room for it. Entered only by far_call; crt0 needs the entries of a
// bank, the gates and the entry frame at the same addresses as the others.
#include <defs.h>
#include "sdr.h"
#include "io_bank.h"

__at (0xBFE0) u16 ret_sp;
__at (0xBFE2) u16 ret_ix;
__at (0xBFE4) u16 ret_iy;
__at (0xBFE6) u32 filesize;
__at (0xBFEA) u8 file_ext;
__at (0xBFEB) u8 call_type;
__at (0xBFEC) u8 gs_loaded;
__at (0xBFED) u8 ft_state;

void main() __naked
{
  __asm
    ret
  __endasm;
}

void main_dispatch()
{
}

void far_entry(IO_SHARED *p)
{
  if (p->op == IO_OPEN) p->ok = sdr_open((u8*)0xC000, p->size) == SDR_OK;
  else if (p->op == IO_READ) p->ok = sdr_read(p->fsec, p->n, p->page, p->offs);
  else p->ok = 0;
}
