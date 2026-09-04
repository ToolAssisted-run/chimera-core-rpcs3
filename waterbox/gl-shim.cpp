// The guest's end of the GPU bridge for RPCS3: the renderer's GL entry points
// are glad pointers, and glad fills them from chimera_gl_lookup, which hands
// out the generated wrappers (generated-gl/gl-bridge-guest.cpp). Each wrapper
// packs its arguments and calls the sandbox's single callback; the host reads
// them in place. The renderer is unmodified and what it believes about the GPU
// is what the driver said, because glGetString crosses the bridge too.
//
// WHAT THIS COSTS: the GPU is outside the sandbox, outside the savestate,
// outside this core's determinism, and different on every machine. A run
// drawn this way replays only on the same driver.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gl-bridge.h" /* miniBox source/gl: the shared contract */

#ifndef CHIMERA_GUEST
#include <EGL/egl.h>
#endif


static chimera_gl_bridge_fn g_bridge;

// THE NATIVE REFERENCE IS ONE BINARY: the renderer, these wrappers and the
// host dispatcher share a single glad table. Installing the wrappers there
// would make the dispatcher call them back into itself forever, so natively
// the renderer resolves the real driver functions and the wrappers stay
// unused; the same GL calls reach the same driver either way. Only the
// sandbox, where the guest's glad is its own, goes through the wrappers.
extern "C" void chimera_rpcs3_install_gpu_bridge(uint64_t addr)
{
  const auto fn = reinterpret_cast<chimera_gl_bridge_fn>(static_cast<uintptr_t>(addr));
  if (!fn)
    return;
#ifdef CHIMERA_GUEST
  if (chimera_gl_install(fn))
    g_bridge = fn;
#else
  g_bridge = fn;
#endif
}

extern "C" int chimera_rpcs3_gpu_bridge_present(void)
{
  return g_bridge != nullptr;
}

// generated-gl/gl-traps.cpp: one trap per name glad knows and the bridge
// does not, each naming itself
extern "C" void* chimera_gl_trap_lookup(const char* name);

extern "C" void chimera_gl_unbridged(const char* name)
{
  fprintf(stderr, "chimera gl: %s was CALLED and has no bridge wrapper - the core-profile\n"
                  "renderer was assumed never to reach it. Add the name to miniBox's master\n"
                  "list (source/gl/gl-entry-points.txt) and rebuild.\n", name);
  abort();
}

namespace
{
[[noreturn]] void UnbridgedCallTrap()
{
  chimera_gl_unbridged("(a name glad does not declare)");
  abort();
}
}  // namespace

// glad asks for every name it knows; a name the bridge has no wrapper for
// gets a trap that says which one, rather than a null glad would take for
// "driver too old"
extern "C" void* chimera_gl_lookup_or_trap(const char* name)
{
#ifdef CHIMERA_GUEST
  if (void* p = chimera_gl_lookup(name))
    return p;
#else
  if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name)))
    return p;
#endif
  if (void* t = chimera_gl_trap_lookup(name))
    return t;
  return reinterpret_cast<void*>(&UnbridgedCallTrap);
}
