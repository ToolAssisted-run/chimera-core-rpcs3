// Guest-only overrides for libc calls whose syscalls the miniBox surface
// rejects (by design: no host filesystem, no host clock, one CPU). One
// static link means defining these here shadows musl's versions.
// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

extern "C" {

// The emulator's own files live in its memory filesystem; anything that
// still reaches libc's directory calls is answered read-only.
int mkdir(const char*, mode_t)
{
  errno = EROFS;
  return -1;
}

int rmdir(const char*)
{
  errno = EROFS;
  return -1;
}

int unlink(const char*)
{
  errno = EROFS;
  return -1;
}

int rename(const char*, const char*)
{
  errno = EROFS;
  return -1;
}

int chmod(const char*, mode_t)
{
  errno = EROFS;
  return -1;
}

int fchmod(int, mode_t)
{
  return 0;
}

// std::thread::hardware_concurrency goes through here; the box is one CPU
// and saying so keeps every pool deterministic.
int sched_getaffinity(pid_t, size_t cpusetsize, cpu_set_t* mask)
{
  if (!mask || cpusetsize < sizeof(unsigned long))
  {
    errno = EINVAL;
    return -1;
  }
  memset(mask, 0, cpusetsize);
  CPU_SET(0, mask);
  return 0;
}

// musl defines all four affinity functions in one object; shadowing one
// means providing all of them.
int sched_setaffinity(pid_t, size_t, const cpu_set_t*)
{
  return 0;
}

int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t*)
{
  return 0;
}

int pthread_getaffinity_np(pthread_t, size_t cpusetsize, cpu_set_t* mask)
{
  return sched_getaffinity(0, cpusetsize, mask);
}

char* getcwd(char* buf, size_t size)
{
  if (!buf || size < 2)
  {
    errno = ERANGE;
    return nullptr;
  }
  buf[0] = '/';
  buf[1] = '\0';
  return buf;
}

int uname(struct utsname* u)
{
  if (!u)
  {
    errno = EFAULT;
    return -1;
  }
  memset(u, 0, sizeof *u);
  strcpy(u->sysname, "Chimera");
  strcpy(u->nodename, "waterbox");
  strcpy(u->release, "1");
  strcpy(u->version, "1");
  strcpy(u->machine, "x86_64");
  return 0;
}

// Priorities and names are host scheduler business; there is none.
int pthread_setschedparam(pthread_t, int, const struct sched_param*)
{
  return 0;
}

int pthread_getschedparam(pthread_t, int* policy, struct sched_param* p)
{
  if (policy)
    *policy = 0;
  if (p)
    p->sched_priority = 0;
  return 0;
}

int sched_get_priority_max(int)
{
  return 0;
}

int sched_get_priority_min(int)
{
  return 0;
}

// The one entropy syscall: never used for machine state (patch 0009), but
// a library may still ask - a fixed stream is the honest answer.
ssize_t getrandom(void* buf, size_t n, unsigned)
{
  static uint64_t x = 0x9E3779B97F4A7C15ull;
  uint8_t* p = (uint8_t*)buf;
  for (size_t i = 0; i < n; i++)
  {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    p[i] = (uint8_t)x;
  }
  return (ssize_t)n;
}

}  // extern "C"

// The dynamic loader: there is none in a static guest, and musl's stand-ins
// say "Dynamic loading not supported", which LLVM's JIT takes as a fatal error
// when it asks to load the process's own symbols (dlopen(NULL)). The process
// IS the library: answer that with a handle, and resolve the C runtime names
// generated code may call by lowering (memcpy and friends) from this binary.
#include <cmath>
#include <dlfcn.h>

namespace
{
struct guest_symbol
{
  const char* name;
  void* address;
};
const guest_symbol g_guest_symbols[] = {
  {"memcpy", reinterpret_cast<void*>(&memcpy)},
  {"memset", reinterpret_cast<void*>(&memset)},
  {"memmove", reinterpret_cast<void*>(&memmove)},
  {"memcmp", reinterpret_cast<void*>(&memcmp)},
  {"strlen", reinterpret_cast<void*>(&strlen)},
  {"fmod", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(&fmod))},
  {"fmodf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(&fmodf))},
  {"sqrt", reinterpret_cast<void*>(static_cast<double (*)(double)>(&sqrt))},
  {"sqrtf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&sqrtf))},
  {"floor", reinterpret_cast<void*>(static_cast<double (*)(double)>(&floor))},
  {"floorf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&floorf))},
  {"ceil", reinterpret_cast<void*>(static_cast<double (*)(double)>(&ceil))},
  {"ceilf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&ceilf))},
  {"trunc", reinterpret_cast<void*>(static_cast<double (*)(double)>(&trunc))},
  {"truncf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&truncf))},
  {"round", reinterpret_cast<void*>(static_cast<double (*)(double)>(&round))},
  {"roundf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&roundf))},
  {"rint", reinterpret_cast<void*>(static_cast<double (*)(double)>(&rint))},
  {"rintf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&rintf))},
  {"nearbyint", reinterpret_cast<void*>(static_cast<double (*)(double)>(&nearbyint))},
  {"nearbyintf", reinterpret_cast<void*>(static_cast<float (*)(float)>(&nearbyintf))},
  {"fma", reinterpret_cast<void*>(static_cast<double (*)(double, double, double)>(&fma))},
  {"fmaf", reinterpret_cast<void*>(static_cast<float (*)(float, float, float)>(&fmaf))},
  {"exp2", reinterpret_cast<void*>(static_cast<double (*)(double)>(&exp2))},
  {"exp2f", reinterpret_cast<void*>(static_cast<float (*)(float)>(&exp2f))},
  {"log2", reinterpret_cast<void*>(static_cast<double (*)(double)>(&log2))},
  {"log2f", reinterpret_cast<void*>(static_cast<float (*)(float)>(&log2f))},
};
int g_process_handle;
}  // namespace

extern "C" {

void* dlopen(const char* file, int)
{
  return file == nullptr ? &g_process_handle : nullptr;
}

void* dlsym(void*, const char* name)
{
  for (const auto& s : g_guest_symbols)
    if (strcmp(s.name, name) == 0)
      return s.address;
  return nullptr;
}

int dlclose(void*)
{
  return 0;
}

char* dlerror(void)
{
  static char message[] = "no dynamic loading in a sandbox";
  return message;
}

}  // extern "C"
