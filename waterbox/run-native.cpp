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
    else
      game = argv[i];
  }
  if (!game)
  {
    fprintf(stderr, "usage: run-native [--work D] [--firmware PS3UPDAT.PUP] [--frames N] [--report N] [--tty-out F] <game.elf|iso>\n");
    return 2;
  }
  // CHIMERA_ALARM=<seconds>: a SIGALRM after that long, so a hang under gdb
  // shows its stacks (gdb stops on the signal)
  if (getenv("CHIMERA_ALARM"))
  {
    signal(SIGALRM, on_alarm);
    alarm(atoi(getenv("CHIMERA_ALARM")));
  }
  if (!chimera_rpcs3_init(work, game, firmware))
  {
    fprintf(stderr, "init failed: %s\n", chimera_rpcs3_error());
    return 1;
  }
  printf("booted; threads %d firmware %s\n", chimera_rpcs3_thread_count(), chimera_rpcs3_firmware_version()[0] ? chimera_rpcs3_firmware_version() : "none");
  fflush(stdout);

  for (long f = 1; f <= frames; f++)
  {
    chimera_rpcs3_frame();
    if (f % report == 0 || f == frames)
    {
      int64_t tn;
      const uint8_t* tty = chimera_rpcs3_tty(&tn);
      printf("frame %5ld ram %016" PRIx64 " tty %" PRId64 " %016" PRIx64 " threads %d time %" PRIu64 " running %d\n",
             f, chimera_rpcs3_main_memory_digest(), tn, fnv(tty, tn), chimera_rpcs3_thread_count(),
             chimera_rpcs3_machine_time_ns() / 1000, chimera_rpcs3_is_running());
      fflush(stdout);
    }
  }
  if (getenv("CHIMERA_DEBUG"))
    chimera_rpcs3_debug_ppu();
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
