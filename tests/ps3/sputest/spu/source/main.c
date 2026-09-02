// The SPU side: for iteration n, n rounds of an LCG over a 16 KiB buffer in
// local store, then a checksum of the buffer, sent back as a user event.
// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>

#define SPUP 10

static uint32_t buffer[4096];

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  (void)arg1; (void)arg2; (void)arg3; (void)arg4;
  for (;;)
  {
    uint32_t n = spu_read_signal1();
    if (n == 0)
      break;
    uint32_t x = n * 2654435761u + 12345u;
    for (uint32_t round = 0; round < (n & 15) + 1; round++)
    {
      for (uint32_t i = 0; i < 4096; i++)
      {
        x = x * 1664525u + 1013904223u;
        buffer[i] = (buffer[i] ^ x) + round;
      }
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 4096; i++)
      sum = sum * 31 + buffer[i];
    spu_thread_send_event(SPUP, n & 0xffffff, sum);
  }
  spu_thread_exit(0);
  return 0;
}
