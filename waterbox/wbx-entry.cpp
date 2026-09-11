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

extern "C" void chimera_rpcs3_install_gpu_bridge(uint64_t addr);

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

  // the firmware channel mounts the PUP under its declared id
  const char* firmware = nullptr;
  FILE* fw = fopen("PS3UPDAT.PUP", "rb");
  if (fw)
  {
    fclose(fw);
    firmware = "PS3UPDAT.PUP";
  }
  // the disc key slot ({"dkey":["name"]}), optional
  char dkeyName[256] = "";
  const char* dkey = wbx_slot_first("dkey", dkeyName, sizeof dkeyName) ? dkeyName : nullptr;
  if (!dkey)
  {
    FILE* dk = fopen("dkey", "rb");
    if (dk)
    {
      fclose(dk);
      dkey = "dkey";
    }
  }
  // which renderer the project asked for; "opengl-hw" only draws when the
  // host also handed over a GPU bridge (SetGpuBridge, before Init)
  char renderer[32] = "null";
  wbx_setting_str("renderer", renderer, sizeof renderer);
  chimera_rpcs3_set_renderer(renderer);
  char spuDecoder[16] = "asmjit";
  wbx_setting_str("spu_decoder", spuDecoder, sizeof spuDecoder);
  chimera_rpcs3_set_spu_decoder(spuDecoder);
  char ppuDecoder[16] = "interpreter";
  wbx_setting_str("ppu_decoder", ppuDecoder, sizeof ppuDecoder);
  chimera_rpcs3_set_ppu_decoder(ppuDecoder);

  // A disc is a retail game, and a retail game links against the system
  // software: without the firmware it boots into nothing, a machine that runs
  // and never polls or draws. Refusing here names the cause; the firmware
  // declaration stays optional because homebrew ELFs boot without it.
  if (!firmware)
  {
    const size_t n = strlen(romName);
    if (n >= 4 && (strcmp(romName + n - 4, ".iso") == 0 || strcmp(romName + n - 4, ".ISO") == 0))
    {
      snprintf(g_loadError, sizeof g_loadError,
               "a PS3 disc needs the system software (PS3UPDAT.PUP), and none was provided");
      return 0;
    }
  }
  if (!chimera_rpcs3_init(nullptr, romName, firmware, dkey))
  {
    snprintf(g_loadError, sizeof g_loadError, "%s", chimera_rpcs3_error());
    return 0;
  }
  return 1;
}

// The GPU bridge: the host's dispatcher, handed over before Init when the
// frontend has a real GL context to offer (see waterbox/gl-shim.cpp)
ECL_EXPORT void SetGpuBridge(uint64_t addr)
{
  chimera_rpcs3_install_gpu_bridge(addr);
}

// miniBox calls this on the faulting thread when a guest instruction hits a
// page this guest protected; nonzero means retry the access
ECL_EXPORT int GuestFaultHandler(uint64_t addr, uint64_t is_write)
{
  return chimera_rpcs3_on_fault(addr, is_write != 0);
}

// how many guest faults the renderer handled so far (diagnostic)
ECL_EXPORT uint64_t GetFaultCount(void)
{
  return chimera_rpcs3_fault_count();
}

// Diagnosis, never machine state: every CPU thread, where it is and what it
// last called, to the host's stderr. For a machine that has gone quiet - a
// PPU parked in an lv2 wait names the HLE function it is parked in.
ECL_EXPORT void DebugThreads(void)
{
  chimera_rpcs3_debug_threads();
}

// The compile cache bridge: the host's dispatcher, handed over before Init
// when the frontend keeps compiled objects for this core (cache-bridge.h)
ECL_EXPORT void SetCacheBridge(uint64_t addr)
{
  chimera_rpcs3_install_cache_bridge(addr);
}

ECL_EXPORT uint64_t GetCacheFetched(void)
{
  return chimera_rpcs3_cache_fetched();
}

ECL_EXPORT uint64_t GetCacheStored(void)
{
  return chimera_rpcs3_cache_stored();
}

// A precompile session: set before Init; the host then pumps FrameAdvance
// until IsPrecompileDone, reading GetPrecompileDone/Total for its bar
ECL_EXPORT void SetPrecompile(int32_t index, int32_t count, int32_t firmware_too)
{
  chimera_rpcs3_set_precompile(index, count, firmware_too);
}

ECL_EXPORT int IsPrecompileDone(void)
{
  return chimera_rpcs3_precompile_done();
}

static uint32_t g_pre_done, g_pre_total;

ECL_EXPORT uint32_t GetPrecompileDone(void)
{
  chimera_rpcs3_precompile_progress(&g_pre_done, &g_pre_total);
  return g_pre_done;
}

ECL_EXPORT uint32_t GetPrecompileTotal(void)
{
  chimera_rpcs3_precompile_progress(&g_pre_done, &g_pre_total);
  return g_pre_total;
}

ECL_EXPORT int IsGpuActive(void)
{
  return chimera_rpcs3_gpu_active();
}

ECL_EXPORT void FrameAdvance(uint64_t /*input*/)
{
  chimera_rpcs3_frame();
}

// 7 ports x 17 buttons, 7 ports x 4 axes, in the driver's wire order
ECL_EXPORT int IsButtonActive(int32_t index)
{
  return index >= 0 && index < 7 * 17 && chimera_rpcs3_port_present(index / 17);
}

ECL_EXPORT int IsAxisActive(int32_t index)
{
  return index >= 0 && index < 7 * 4 && chimera_rpcs3_port_present(index / 4);
}

ECL_EXPORT void SetButton(int32_t index, int32_t state)
{
  chimera_rpcs3_set_button(index / 17, index % 17, state);
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
  // the frontend's signed axis (-128..127) onto the pad's byte
  chimera_rpcs3_set_axis(index / 4, index % 4, value + 128);
}

ECL_EXPORT int InputWasRead(void)
{
  return chimera_rpcs3_input_was_read();
}

static int g_vw, g_vh, g_an;

ECL_EXPORT uint32_t* GetVideoBgra(void)
{
  return const_cast<uint32_t*>(chimera_rpcs3_video(&g_vw, &g_vh));
}

ECL_EXPORT int GetVideoWidth(void)
{
  chimera_rpcs3_video(&g_vw, &g_vh);
  return g_vw;
}

ECL_EXPORT int GetVideoHeight(void)
{
  chimera_rpcs3_video(&g_vw, &g_vh);
  return g_vh;
}

ECL_EXPORT int16_t* GetAudio(void)
{
  return const_cast<int16_t*>(chimera_rpcs3_audio(&g_an));
}

ECL_EXPORT int GetAudioSampleCount(void)
{
  chimera_rpcs3_audio(&g_an);
  return g_an;
}

ECL_EXPORT int GetVsyncNumerator(void)
{
  return chimera_rpcs3_vsync_numerator();
}

ECL_EXPORT int GetVsyncDenominator(void)
{
  return chimera_rpcs3_vsync_denominator();
}

// The PS3's main memory block: 0x00010000 for 0x0FFF0000 bytes, mapped whole
// at init and never unmapped, so the frontend reads the guest view directly.
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
  return chimera_rpcs3_main_memory_ptr();
}

ECL_EXPORT int64_t GetMemoryDomainSize(int)
{
  return 0x0FFF0000;
}

ECL_EXPORT int GetMemoryDomainWritable(int)
{
  return 1;
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
