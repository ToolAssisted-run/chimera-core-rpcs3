// First-light runner for the native reference: boot, step N frames, print a
// running FNV-1a digest of main memory and of the TTY. Two invocations
// printing the same lines is the determinism check; the guest printing them
// too is M1.
// SPDX-License-Identifier: MIT

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rpcs3-driver.h"

#include <unistd.h>
#include <csignal>

#ifdef CHIMERA_GL_BRIDGE
// the host half of the GPU bridge, in this same binary (waterbox/gl-host.c)
extern "C" int chimera_gl_host_init(char* err, int errlen);
extern "C" const char* chimera_gl_host_description(void);
extern "C" uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
extern "C" void chimera_rpcs3_install_gpu_bridge(uint64_t addr);
#endif

static void on_alarm(int)
{
  chimera_rpcs3_debug_ppu();
  _exit(9);
}

static uint64_t fnv(const uint8_t* p, int64_t n)
{
  uint64_t h = 1469598103934665603ULL;
  for (int64_t i = 0; i < n; i++)
  {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

int main(int argc, char** argv)
{
  const char* game = nullptr;
  const char* work = "work";
  long frames = 60;
  long report = 10;
  const char* tty_out = nullptr;
  const char* firmware = nullptr;
  const char* dkey = nullptr;
  const char* videoOut = nullptr;
  struct { long first, count; int index; } press[32];
  int presses = 0;
  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc)
      frames = atol(argv[++i]);
    else if (!strcmp(argv[i], "--report") && i + 1 < argc)
      report = atol(argv[++i]);
    else if (!strcmp(argv[i], "--work") && i + 1 < argc)
      work = argv[++i];
    else if (!strcmp(argv[i], "--tty-out") && i + 1 < argc)
      tty_out = argv[++i];
    else if (!strcmp(argv[i], "--firmware") && i + 1 < argc)
      firmware = argv[++i];
    else if (!strcmp(argv[i], "--dkey") && i + 1 < argc)
      dkey = argv[++i];
    else if (!strcmp(argv[i], "--video-out") && i + 1 < argc)
      videoOut = argv[++i];
    else if (!strcmp(argv[i], "--renderer") && i + 1 < argc)
    {
      const char* r = argv[++i];
      chimera_rpcs3_set_renderer(r);
#ifdef CHIMERA_GL_BRIDGE
      if (!strcmp(r, "opengl-hw") || !strcmp(r, "opengl"))
      {
        // Install BEFORE the host context loads its entry points: in this
        // single binary the guest install writes wrappers into the shared
        // glad table, and gladLoadGL afterwards puts the REAL driver
        // functions back - the host dispatch must call those, or every call
        // recurses through its own wrapper forever.
        chimera_rpcs3_install_gpu_bridge((uint64_t)(uintptr_t)&chimera_gl_host_dispatch);
        char glerr[256] = "";
        if (chimera_gl_host_init(glerr, sizeof glerr) != 0)
          fprintf(stderr, "gpu bridge: no context (%s)\n", glerr);
        else
          fprintf(stderr, "gpu bridge: %s\n", chimera_gl_host_description());
      }
#endif
    }
    else if (!strcmp(argv[i], "--ports") && i + 1 < argc)
    {
      const char* mask = argv[++i];  // e.g. "1010000": ports 1 and 3
      for (int pi = 0; pi < 7 && mask[pi]; pi++)
        chimera_rpcs3_set_port(pi, mask[pi] == '1');
    }
    else if (!strcmp(argv[i], "--press") && i + 1 < argc && presses < 32)
    {
      long a, b;
      int c;
      if (sscanf(argv[++i], "%ld:%ld:%d", &a, &b, &c) == 3)
      {
        press[presses].first = a;
        press[presses].count = b;
        press[presses].index = c;
        presses++;
      }
    }
    else
      game = argv[i];
  }
  if (!game)
  {
    fprintf(stderr, "usage: run-native [--work D] [--firmware PS3UPDAT.PUP] [--dkey game.dkey] [--renderer null|opengl-hw] [--frames N] [--report N] [--tty-out F] [--video-out F] [--press first:count:index] [--ports 1000000] <game.elf|iso>\n");
    return 2;
  }
  // CHIMERA_ALARM=<seconds>: a SIGALRM after that long, so a hang under gdb
  // shows its stacks (gdb stops on the signal)
  if (getenv("CHIMERA_ALARM"))
  {
    signal(SIGALRM, on_alarm);
    alarm(atoi(getenv("CHIMERA_ALARM")));
  }
  // a crash reports the PPU state the same way (the emulator's own handler
  // is off in this build, patch 0003)
  signal(SIGSEGV, on_alarm);
  if (!chimera_rpcs3_init(work, game, firmware, dkey))
  {
    fprintf(stderr, "init failed: %s\n", chimera_rpcs3_error());
    return 1;
  }
  printf("booted; threads %d firmware %s\n", chimera_rpcs3_thread_count(), chimera_rpcs3_firmware_version()[0] ? chimera_rpcs3_firmware_version() : "none");
  fflush(stdout);

  long lag = 0;
  for (long f = 1; f <= frames; f++)
  {
    for (int pi = 0; pi < presses; pi++)
      chimera_rpcs3_set_button(0, press[pi].index, f >= press[pi].first && f < press[pi].first + press[pi].count);
    chimera_rpcs3_frame();
    if (!chimera_rpcs3_input_was_read())
      lag++;
    if (f % report == 0 || f == frames)
    {
      int64_t tn;
      const uint8_t* tty = chimera_rpcs3_tty(&tn);
      int vw, vh, an;
      const uint32_t* vid = chimera_rpcs3_video(&vw, &vh);
      const int16_t* aud = chimera_rpcs3_audio(&an);
      printf("frame %5ld ram %016" PRIx64 " tty %" PRId64 " %016" PRIx64 " vid %dx%d %016" PRIx64 " aud %d %016" PRIx64 " lag %ld threads %d time %" PRIu64 " running %d\n",
             f, chimera_rpcs3_main_memory_digest(), tn, fnv(tty, tn), vw, vh, fnv((const uint8_t*)vid, (int64_t)vw * vh * 4),
             an, fnv((const uint8_t*)aud, (int64_t)an * 4), lag, chimera_rpcs3_thread_count(),
             chimera_rpcs3_machine_time_ns() / 1000, chimera_rpcs3_is_running());
      fflush(stdout);
    }
  }
  if (getenv("CHIMERA_DEBUG"))
    chimera_rpcs3_debug_ppu();
  if (videoOut)
  {
    // the last frame's picture: two little-endian u32 (width, height), then
    // BGRA rows top-down, for a script to look at
    int vw = 0, vh = 0;
    const uint32_t* px = chimera_rpcs3_video(&vw, &vh);
    FILE* f = fopen(videoOut, "wb");
    if (f)
    {
      uint32_t hdr[2] = {(uint32_t)vw, (uint32_t)vh};
      fwrite(hdr, 4, 2, f);
      if (px && vw > 0 && vh > 0)
        fwrite(px, 4, (size_t)vw * vh, f);
      fclose(f);
    }
  }
  if (tty_out)
  {
    int64_t tn;
    const uint8_t* tty = chimera_rpcs3_tty(&tn);
    FILE* f = fopen(tty_out, "wb");
    if (f)
    {
      fwrite(tty, 1, (size_t)tn, f);
      fclose(f);
    }
  }
  chimera_rpcs3_shutdown();
  printf("done\n");
  fflush(stdout);
  // the emulator library's static destructors expect a teardown its own
  // main() never performs either; the machine is stopped, leave now
  _exit(0);
}
