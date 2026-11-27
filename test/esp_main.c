// Test program of the ESP audio client (src/esp.c): runs in the Z80 model of
// test/test_esp.py. The model writes the operations into t_ops (STAGE: ptr is
// an offset into t_data) and reads them back after main returns.
#include <defs.h>
#include "esp.h"

void esp_entry(ESP_SHARED *p);

u8 t_n;
// The data below the ESP ring (#AC00..#BC00), the variables above it.
ESP_SHARED t_ops[40];
__at (0x9000) u8 t_data[0x1800];

void main()
{
  u8 i;
  for (i = 0; i < t_n; i++)
  {
    if (t_ops[i].op == ESP_STAGE) t_ops[i].ptr += (u16)t_data;
    esp_entry(&t_ops[i]);
  }
}
