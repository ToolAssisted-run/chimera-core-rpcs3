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
#include <vector>

#include "rpcs3-driver.h"

#include <unistd.h>
#include <csignal>
#include <cstring>
#include <ucontext.h>

// the host half of the compile cache, in this same binary (waterbox/cache-host.c)
extern "C" int chimera_cache_host_init(const char* dir);
extern "C" const char* chimera_cache_host_description(void);
extern "C" uintptr_t chimera_cache_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);

#ifdef CHIMERA_GL_BRIDGE
// the host half of the GPU bridge, in this same binary (waterbox/gl-host.c)
extern "C" int chimera_gl_host_init(char* err, int errlen);
extern "C" const char* chimera_gl_host_description(void);
extern "C" uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
extern "C" void chimera_rpcs3_install_gpu_bridge(uint64_t addr);
// Moves the context id, which is what the engine does on a state load. The
// renderer notices at its next local task and rebuilds its GL objects. Here so
// that the REBUILD can be driven on its own, with no state restored: the
// renderer's teardown and setup are the half of a load that touches the GL
// objects, and a bug in them is otherwise only reachable through a savestate
// round trip that costs minutes on a real game.
extern "C" void chimera_gl_host_state_loaded(void);
#endif

static void on_alarm(int)
{
  chimera_rpcs3_debug_ppu();
  _exit(9);
}

static void on_fault(int, siginfo_t* info, void* uctx)
{
  const auto* uc = static_cast<ucontext_t*>(uctx);
  const int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
  if (chimera_rpcs3_on_fault(reinterpret_cast<uint64_t>(info->si_addr), is_write))
    return;
  fprintf(stderr, "fault at %p (%s), not the renderer's\n", info->si_addr, is_write ? "write" : "read");
  chimera_rpcs3_debug_ppu();
  _exit(11);
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
  const char* ram_out = nullptr;
  const char* firmware = nullptr;
  const char* dkey = nullptr;
  std::vector<const char*> pkgs;
  std::vector<const char*> raps;
  const char* videoOut = nullptr;
  const char* cacheDir = nullptr;
  // a game's data install in miniature: copy this file off the disc onto the
  // hard disk once the machine is up and read it back. --disc-copy-raw reads
  // the image as it lies (the control), --disc-verify reads back without
  // copying (for a machine that came out of a savestate).
  const char* discCopy = nullptr;
  int discCopyFlags = 0;
  int preIndex = -1, preCount = 0, preFirmware = 1;
  struct { long first, count; int index; } press[32];
  int presses = 0;
  // --gl-rebuild-at N[,N...]: the frames after which the context id moves, so
  // the renderer rebuilds. See the declaration above for why this is separable
  // from a state load at all.
  long rebuildAt[32];
  int rebuilds = 0;
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
    else if (!strcmp(argv[i], "--ram-out") && i + 1 < argc)
      ram_out = argv[++i];
    else if (!strcmp(argv[i], "--firmware") && i + 1 < argc)
      firmware = argv[++i];
    else if (!strcmp(argv[i], "--dkey") && i + 1 < argc)
      dkey = argv[++i];
    else if (!strcmp(argv[i], "--disc-copy") && i + 1 < argc)
      discCopy = argv[++i];
    else if (!strcmp(argv[i], "--disc-copy-raw"))
      discCopyFlags |= 1;
    else if (!strcmp(argv[i], "--disc-verify"))
      discCopyFlags |= 2;
    else if (!strcmp(argv[i], "--pkg") && i + 1 < argc)
      pkgs.push_back(argv[++i]);
    else if (!strcmp(argv[i], "--rap") && i + 1 < argc)
      raps.push_back(argv[++i]);
    else if (!strcmp(argv[i], "--video-out") && i + 1 < argc)
      videoOut = argv[++i];
    else if (!strcmp(argv[i], "--precompile") && i + 1 < argc)
    {
      // INDEX/COUNT[/game]: a precompile session, worker INDEX of COUNT; "game"
      // limits the sweep to the game's directories (no firmware libraries)
      const char* spec = argv[++i];
      if (sscanf(spec, "%d/%d", &preIndex, &preCount) != 2 || preIndex < 0 || preCount < 1 || preIndex >= preCount)
      {
        fprintf(stderr, "bad --precompile %s (want INDEX/COUNT)\n", spec);
        return 2;
      }
      preFirmware = strstr(spec, "/game") == nullptr;
      chimera_rpcs3_set_precompile(preIndex, preCount, preFirmware);
    }
    else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
    {
      cacheDir = argv[++i];
      if (chimera_cache_host_init(cacheDir) != 0)
        fprintf(stderr, "compile cache: cannot use %s\n", cacheDir);
      else
        chimera_rpcs3_install_cache_bridge((uint64_t)(uintptr_t)&chimera_cache_host_dispatch);
    }
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
    else if (!strcmp(argv[i], "--gl-rebuild-at") && i + 1 < argc)
    {
      for (const char* p = argv[++i]; *p && rebuilds < 32;)
      {
        rebuildAt[rebuilds++] = atol(p);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
      }
    }
    else
      game = argv[i];
  }
  if (!game)
  {
    fprintf(stderr, "usage: run-native [--work D] [--firmware PS3UPDAT.PUP] [--dkey game.dkey] [--pkg file.pkg]... [--rap licence.rap]... [--renderer null|opengl-hw] [--frames N] [--report N] [--tty-out F] [--ram-out F] [--video-out F] [--cache DIR] [--precompile INDEX/COUNT[/game]] [--press first:count:index] [--ports 1000000] [--gl-rebuild-at N[,N...]] [--disc-copy PATH/ON/DISC [--disc-copy-raw] [--disc-verify]] <game.elf|iso>\n");
    return 2;
  }
  // CHIMERA_ALARM=<seconds>: a SIGALRM after that long, so a hang under gdb
  // shows its stacks (gdb stops on the signal)
  if (getenv("CHIMERA_ALARM"))
  {
    signal(SIGALRM, on_alarm);
    alarm(atoi(getenv("CHIMERA_ALARM")));
  }
  // a fault on a guest page goes to the renderer's caches first (the
  // emulator's own handler is off in this build, patch 0003); anything else
  // reports the PPU state the way the alarm does
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
  }
  for (const char* pkg : pkgs)
    chimera_rpcs3_add_package(pkg);
  for (const char* rap : raps)
    chimera_rpcs3_add_rap(rap);
  if (!chimera_rpcs3_init(work, game, firmware, dkey))
  {
    fprintf(stderr, "init failed: %s\n", chimera_rpcs3_error());
    return 1;
  }
  printf("booted; threads %d firmware %s\n", chimera_rpcs3_thread_count(), chimera_rpcs3_firmware_version()[0] ? chimera_rpcs3_firmware_version() : "none");
  fflush(stdout);

  long lag = 0;
  if (preCount > 0)
  {
    // a precompile session: pump until the sweep is done, one line per change
    uint32_t last_done = ~0u;
    while (!chimera_rpcs3_precompile_done())
    {
      chimera_rpcs3_frame();
      uint32_t done = 0, total = 0;
      chimera_rpcs3_precompile_progress(&done, &total);
      if (done != last_done)
      {
        fprintf(stderr, "Precompiled %u/%u modules\n", done, total);
        last_done = done;
      }
    }
    uint32_t done = 0, total = 0;
    chimera_rpcs3_precompile_progress(&done, &total);
    printf("precompiled %u/%u modules (worker %d of %d)\n", done, total, preIndex, preCount);
    frames = 0;
  }
  // The copy before the frames, as in run-wbx, so the two runners ask the same
  // question of the same machine - and its figures printed HERE, with nothing
  // else having run since, because the emulator appends to its own log in the
  // memory filesystem and a leg that read the figures at the end of the run
  // would be reading the log's growth as well as the copy's cost.
  if (discCopy)
  {
    const long long copied = chimera_rpcs3_disc_copy_probe(discCopy, discCopyFlags);
    printf("disc copy %s: %lld bytes, %llu held, %llu kept, %llu decrypted\n", discCopy, copied,
           (unsigned long long)chimera_rpcs3_memfs_stat(2), (unsigned long long)chimera_rpcs3_memfs_stat(3),
           (unsigned long long)chimera_rpcs3_memfs_stat(4));
    fflush(stdout);
  }
  for (long f = 1; f <= frames; f++)
  {
    for (int pi = 0; pi < presses; pi++)
      chimera_rpcs3_set_button(press[pi].index / 17, press[pi].index % 17, f >= press[pi].first && f < press[pi].first + press[pi].count);  // the wire index, as SetButton takes it
    chimera_rpcs3_frame();
    if (!chimera_rpcs3_input_was_read())
      lag++;
#ifdef CHIMERA_GL_BRIDGE
    for (int ri = 0; ri < rebuilds; ri++)
      if (rebuildAt[ri] == f)
      {
        fprintf(stderr, "gl: the context id moves after frame %ld; the renderer will rebuild\n", f);
        fflush(stderr);
        chimera_gl_host_state_loaded();
      }
#endif
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
  // CHIMERA_DEBUG_THREADS=1: the same report run-wbx's DebugThreads gives -
  // every CPU thread, where it is and what lv2 wait it is parked in - for a
  // machine that has gone quiet by the end of the run.
  if (getenv("CHIMERA_DEBUG_THREADS"))
    chimera_rpcs3_debug_threads();
  // CHIMERA_DISASM=0xADDR:N[,0xADDR:N...]: the guest's own instructions there,
  // for a thread that is spinning rather than parked - the loop says what it
  // polls and nothing else does.
  if (const char* spec = getenv("CHIMERA_DISASM"))
  {
    unsigned long a = 0;
    int n = 0;
    while (*spec)
    {
      if (sscanf(spec, "%lx:%d", &a, &n) == 2 || sscanf(spec, "0x%lx:%d", &a, &n) == 2)
      {
        fprintf(stderr, "== guest code at %08lx (%d)\n", a, n);
        chimera_rpcs3_disasm((uint32_t)a, n);
      }
      const char* comma = strchr(spec, ',');
      if (!comma)
        break;
      spec = comma + 1;
    }
  }
  // CHIMERA_PEEK=[@]ADDR[+OFF]:N[,...]: guest memory as big-endian words; the
  // leading @ dereferences ADDR first, so a field of an object a global points
  // at can be read without a second run.
  if (const char* spec = getenv("CHIMERA_PEEK"))
  {
    while (*spec)
    {
      const int deref = (*spec == '@');
      const char* p = spec + deref;
      unsigned long a = 0;
      long off = 0;
      int n = 0;
      if (sscanf(p, "%lx+%lx:%d", &a, (unsigned long*)&off, &n) == 3 || sscanf(p, "%lx:%d", &a, &n) == 2)
      {
        fprintf(stderr, "== guest memory at %s%08lx+%ld (%d)\n", deref ? "@" : "", a, off, n);
        chimera_rpcs3_peek((uint32_t)a, (int32_t)off, n, deref);
      }
      const char* comma = strchr(spec, ',');
      if (!comma)
        break;
      spec = comma + 1;
    }
  }
  if (cacheDir)
    fprintf(stderr, "compile cache: %llu stored, %llu fetched (%s)\n", (unsigned long long)chimera_rpcs3_cache_stored(), (unsigned long long)chimera_rpcs3_cache_fetched(), chimera_cache_host_description());
  if (chimera_rpcs3_fault_count())
    fprintf(stderr, "page faults served by the renderer: %llu\n", (unsigned long long)chimera_rpcs3_fault_count());
  if (chimera_rpcs3_window_count())
    fprintf(stderr, "faults of the RSX's own served by opening the page: %llu\n", (unsigned long long)chimera_rpcs3_window_count());
  fprintf(stderr, "memory files: %llu bytes, %llu file(s) held as the disc's (%llu bytes), %llu bytes copied in, %llu bytes decrypted to tell\n",
          (unsigned long long)chimera_rpcs3_memfs_stat(0), (unsigned long long)chimera_rpcs3_memfs_stat(1),
          (unsigned long long)chimera_rpcs3_memfs_stat(2), (unsigned long long)chimera_rpcs3_memfs_stat(3),
          (unsigned long long)chimera_rpcs3_memfs_stat(4));
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
  // the same memory domain run-wbx writes, so two flavors that disagree on a
  // digest can be diffed rather than guessed about
  if (ram_out)
  {
    FILE* f = fopen(ram_out, "wb");
    if (f)
    {
      fwrite(chimera_rpcs3_main_memory_ptr(), 1, 0x0FFF0000, f);
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
