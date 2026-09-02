// The chimera guest ABI layer: wraps rpcs3-driver into the miniBox core ABI
// (the same export surface as the other chimera cores). Compiled ONLY for
// the guest; run-native.cpp is the native twin.
//
// M1 scope: boot + frame advance + the main-memory domain. Video, audio,
// input, savedata and settings arrive with their milestones.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "rpcs3-driver.h"

static char g_loadError[512];

extern "C" {

ECL_EXPORT const char* GetLoadError(void)
{
  return g_loadError;
}

ECL_EXPORT int Init(void)
{
  g_loadError[0] = '\0';

  // The game to boot: the project mounts "slots" ({"game":["name"]}, the
  // file itself under that canonical name); a rom opened directly arrives
  // as "rom" with its real name in "rom.name" so extension detection still
  // works.
  char romName[256] = "game";
  if (!wbx_slot_first("game", romName, sizeof romName))
  {
    char realName[256] = "";
    FILE* f = fopen("rom.name", "rb");
    if (f)
    {
      size_t n = fread(realName, 1, sizeof realName - 1, f);
      while (n && (realName[n - 1] == '\n' || realName[n - 1] == '\r'))
        n--;
      realName[n] = '\0';
      fclose(f);
    }
    char candidate[300];
    snprintf(candidate, sizeof candidate, "%s%s", realName[0] == '/' ? "" : "/", realName);
    FILE* probe = realName[0] ? fopen(candidate, "rb") : nullptr;
    if (!probe && realName[0])
      probe = fopen(realName, "rb"), snprintf(candidate, sizeof candidate, "%s", realName);
    if (probe)
    {
      fclose(probe);
      snprintf(romName, sizeof romName, "%s", candidate);
    }
  }

  if (!chimera_rpcs3_init(nullptr, romName))
  {
    snprintf(g_loadError, sizeof g_loadError, "%s", chimera_rpcs3_error());
    return 0;
  }
  return 1;
}

ECL_EXPORT void FrameAdvance(uint64_t /*input*/)
{
  chimera_rpcs3_frame();
}

ECL_EXPORT int InputWasRead(void)
{
  return 1;
}

ECL_EXPORT int GetVsyncNumerator(void)
{
  return chimera_rpcs3_vsync_numerator();
}

ECL_EXPORT int GetVsyncDenominator(void)
{
  return chimera_rpcs3_vsync_denominator();
}

// The PS3's main memory as the machine sees it (256 MiB from 0), served
// through a copy the harness can hash: the flat guest view has unmapped
// holes and those read as zero.
static uint8_t* g_ram_copy;

ECL_EXPORT int GetMemoryDomainCount(void)
{
  return 1;
}

ECL_EXPORT const char* GetMemoryDomainName(int)
{
  return "MainRAM";
}

ECL_EXPORT uint8_t* GetMemoryDomainPtr(int)
{
  if (!g_ram_copy)
    g_ram_copy = (uint8_t*)alloc_invisible(256u << 20);
  chimera_rpcs3_read_main_memory(0, 256u << 20, g_ram_copy);
  return g_ram_copy;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int)
{
  return 256 << 20;
}

ECL_EXPORT int GetMemoryDomainWritable(int)
{
  return 0;
}

ECL_EXPORT uint64_t GetMainMemoryDigest(void)
{
  return chimera_rpcs3_main_memory_digest();
}

static int64_t g_tty_n;

ECL_EXPORT const uint8_t* GetTty(void)
{
  return chimera_rpcs3_tty(&g_tty_n);
}

ECL_EXPORT int64_t GetTtySize(void)
{
  chimera_rpcs3_tty(&g_tty_n);
  return g_tty_n;
}

ECL_EXPORT int GetThreadCount(void)
{
  return chimera_rpcs3_thread_count();
}

ECL_EXPORT uint64_t GetMachineTimeNs(void)
{
  return chimera_rpcs3_machine_time_ns();
}

ECL_EXPORT int IsRunning(void)
{
  return chimera_rpcs3_is_running();
}

}  // extern "C"
