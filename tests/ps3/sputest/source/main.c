// spu: the PPU keeps one SPU thread busy. Every frame it signals the SPU an
// iteration number; the SPU runs its loops over local store and answers with
// a checksum through a user event; the PPU reports both on the TTY. Every
// answer is a function of the iteration alone, so the same lines must come
// out whichever way the SPU's code is executed, and the frame each answer
// lands on is the scheduler's business.
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <ppu-types.h>
#include <sys/event_queue.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include "spu_bin.h"

#define SPUP 10
#define ptr2ea(x) ((u64)((void*)(x)))

int main(void)
{
  sysSpuImage image;
  u32 thread_id, group_id;
  sys_event_queue_t queue;
  sys_event_t event;
  sys_event_queue_attr_t queue_attr = {SYS_EVENT_QUEUE_FIFO, SYS_EVENT_QUEUE_PPU, "chimeraQ"};
  sysSpuThreadArgument arg = {0, 0, 0, 0};
  sysSpuThreadGroupAttribute group_attr = {8, ptr2ea("chimera"), 0, 0};
  sysSpuThreadAttribute attr = {ptr2ea("worker"), 7, SPU_THREAD_ATTR_NONE};

  sysSpuInitialize(6, 0);
  sysSpuImageImport(&image, spu_bin, 0);
  sysSpuThreadGroupCreate(&group_id, 1, 100, &group_attr);
  sysEventQueueCreate(&queue, &queue_attr, 0x4242, 16);
  sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg);
  sysSpuThreadSetConfiguration(thread_id, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);
  sysSpuThreadConnectEvent(thread_id, queue, SPU_THREAD_EVENT_USER, SPUP);
  sysSpuThreadGroupStart(group_id);
  printf("spu: worker started\n");

  for (u32 n = 1; n < 30000; n++)
  {
    sysSpuThreadWriteSignal(thread_id, 0, n);
    sysEventQueueReceive(queue, &event, 0);
    if (event.source == SPU_THREAD_EVENT_USER_KEY && event.data_1 == thread_id && (event.data_2 >> 32) == SPUP)
      printf("spu %u sum %08x\n", (unsigned)(event.data_2 & 0xffffff), (unsigned)event.data_3);
    else
      printf("spu %u unexpected event\n", (unsigned)n);
    sysUsleep(16666);
  }
  sysSpuThreadWriteSignal(thread_id, 0, 0);
  u32 cause, status;
  sysSpuThreadGroupJoin(group_id, &cause, &status);
  sysSpuThreadGroupDestroy(group_id);
  sysSpuImageClose(&image);
  return 0;
}
