// tone: the PS3 plays a square wave through cellAudio. The program opens a
// two-channel port, keeps the ring ahead of the hardware read index with a
// 440 Hz square on the left and 660 Hz on the right, and reports each
// block it wrote on the TTY. What the frontend receives is the wave, so the
// audio path is checkable sample for sample.
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <audio/audio.h>
#include <ppu-types.h>
#include <sys/systime.h>

static float phase_l, phase_r;

static void fill_block(float* buf)
{
  for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
  {
    buf[i * 2 + 0] = phase_l < 0.5f ? 0.25f : -0.25f;
    buf[i * 2 + 1] = phase_r < 0.5f ? 0.25f : -0.25f;
    phase_l += 440.0f / 48000.0f;
    phase_r += 660.0f / 48000.0f;
    if (phase_l >= 1.0f)
      phase_l -= 1.0f;
    if (phase_r >= 1.0f)
      phase_r -= 1.0f;
  }
}

int main(void)
{
  audioPortParam params;
  audioPortConfig config;
  u32 port;

  int ret = audioInit();
  printf("audioInit %d\n", ret);
  params.numChannels = AUDIO_PORT_2CH;
  params.numBlocks = AUDIO_BLOCK_8;
  params.attrib = 0;
  params.level = 1;
  ret = audioPortOpen(&params, &port);
  printf("audioPortOpen %d port %u\n", ret, (unsigned)port);
  ret = audioGetPortConfig(port, &config);
  printf("audioGetPortConfig %d channels %u blocks %u\n", ret, (unsigned)config.channelCount, (unsigned)config.numBlocks);
  ret = audioPortStart(port);
  printf("audioPortStart %d\n", ret);

  volatile u64* read_index = (volatile u64*)(u64)config.readIndex;
  float* data = (float*)(u64)config.audioDataStart;
  u32 next = 1;
  for (u32 written = 0; written < 30000;)
  {
    u64 current = *read_index;
    if (next == current)
    {
      sysUsleep(500);
      continue;
    }
    fill_block(data + 2 * AUDIO_BLOCK_SAMPLES * next);
    written++;
    if (written % 16 == 0)
      printf("block %u hardware %u\n", (unsigned)written, (unsigned)current);
    next = (next + 1) % AUDIO_BLOCK_8;
  }
  audioPortStop(port);
  audioPortClose(port);
  audioQuit();
  return 0;
}
