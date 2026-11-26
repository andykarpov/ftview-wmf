// Test program of the SD reader: runs in the Z80 model of test/test_sdr.py
// at #8000 like the plugin. The model writes t_size and t_req[] before the
// run and reads the results after main returns.
#include <defs.h>
#include "sdr.h"

typedef struct
{
  u32 fsec;
  u8 count;     // 0 ends the list
  u8 page;
  u16 offs;
  u8 ok;
} T_REQ;

u32 t_size;
u8 t_result;
T_REQ t_req[16];
u8 t_buf[1536];

void main()
{
  T_REQ *r;
  t_result = sdr_open(t_buf, t_size);
  if (t_result != SDR_OK) return;
  for (r = t_req; r->count; r++)
    r->ok = sdr_read(r->fsec, r->count, r->page, r->offs);
}
