// flip: the PS3 draws with the CPU and flips with the RSX. Every frame the
// program fills the back buffer with a pattern that depends on the frame
// number, asks the RSX to flip to it, waits for that flip, and reports on
// the TTY. What the frontend receives at each flip is the pattern itself,
// so the video path is checkable end to end without a rasteriser.
// SPDX-License-Identifier: MIT
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <sys/systime.h>
#include <sysutil/video.h>

#define CB_SIZE   0x100000
#define HOST_SIZE (32 * 1024 * 1024)

static gcmContextData* context;
static u32* fb[2];
static u32 fb_offset[2];
static u32 width, height, pitch;

static u32 fill(u32* buf, u32 frame)
{
  // a diagonal gradient that scrolls with the frame, a bar whose height is
  // the frame number, and a checksum of every pixel written
  u32 sum = 0;
  for (u32 y = 0; y < height; y++)
  {
    u32* row = buf + y * (pitch / 4);
    for (u32 x = 0; x < width; x++)
    {
      u32 r = (x + frame) & 0xff;
      u32 g = (y + frame * 2) & 0xff;
      u32 b = (y < (frame % height)) ? 0xff : 0x20;
      u32 px = (r << 16) | (g << 8) | b;
      row[x] = px;
      sum = sum * 31 + px;
    }
  }
  return sum;
}

int main(void)
{
  void* host_addr = memalign(1024 * 1024, HOST_SIZE);
  rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr);

  videoState state;
  videoResolution res;
  videoGetState(0, 0, &state);
  videoGetResolution(state.displayMode.resolution, &res);

  videoConfiguration vconfig;
  memset(&vconfig, 0, sizeof vconfig);
  vconfig.resolution = state.displayMode.resolution;
  vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
  vconfig.pitch = res.width * sizeof(u32);
  videoConfigure(0, &vconfig, NULL, 0);
  videoGetState(0, 0, &state);
  gcmSetFlipMode(GCM_FLIP_VSYNC);

  width = res.width;
  height = res.height;
  pitch = width * sizeof(u32);
  for (int i = 0; i < 2; i++)
  {
    fb[i] = (u32*)rsxMemalign(64, height * pitch);
    rsxAddressToOffset(fb[i], &fb_offset[i]);
    gcmSetDisplayBuffer(i, fb_offset[i], pitch, width, height);
  }
  printf("flip: %ux%u pitch %u\n", width, height, pitch);

  gcmResetFlipStatus();
  for (u32 frame = 0; frame < 30000; frame++)
  {
    u32 cur = frame & 1;
    u32 sum = fill(fb[cur], frame);
    gcmSetFlip(context, cur);
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
    // the flip status clears when the RSX has shown the buffer
    while (gcmGetFlipStatus() != 0)
      sysUsleep(200);
    gcmResetFlipStatus();
    printf("frame %u buffer %u sum %08x\n", (unsigned)frame, (unsigned)cur, (unsigned)sum);
  }
  return 0;
}
