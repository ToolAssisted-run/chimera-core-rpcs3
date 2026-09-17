// Host plumbing the machine has none of, in BOTH flavors so the native
// reference and the sandbox start exactly the same threads: libusb's event
// pipe, curl's socket pair, sockets, the audio timer's epoll, process CPU
// accounting. Each caller copes with the failure (no USB devices, no
// network, the virtual timer instead). Definitions in the executable win
// over libc's, natively and in the static guest link alike.
// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstring>
#include <sys/resource.h>
#include <sys/times.h>

extern "C" {
int pipe(int*)
{
  errno = ENOSYS;
  return -1;
}
int pipe2(int*, int)
{
  errno = ENOSYS;
  return -1;
}
int socketpair(int, int, int, int*)
{
  errno = ENOSYS;
  return -1;
}
int socket(int, int, int)
{
  // Not ENOSYS: RPCS3 hands a failed socket's errno straight to
  // sys_net's convert_error, whose switch knows only the errnos a real
  // socket layer returns. An unmapped one is a thrown exception, which
  // kills the PPU thread and leaves every other thread waiting on it -
  // a whole-core deadlock from a call that merely had no answer
  // (chimera issue #84, Prince of Persia's libnet thread at frame 647).
  // ENETDOWN is in that map and is the truth about this machine: there
  // is no network here. The game gets a failed socket and carries on.
  errno = ENETDOWN;
  return -1;
}
int eventfd(unsigned, int)
{
  errno = ENOSYS;
  return -1;
}
int epoll_create(int)
{
  errno = ENOSYS;
  return -1;
}
int epoll_create1(int)
{
  errno = ENOSYS;
  return -1;
}
int timerfd_create(int, int)
{
  errno = ENOSYS;
  return -1;
}
}

// Process CPU accounting: the machine keeps none.
extern "C" clock_t times(struct tms* t)
{
  if (t)
    memset(t, 0, sizeof *t);
  return 0;
}

extern "C" int getrusage(int, struct rusage* r)
{
  if (r)
    memset(r, 0, sizeof *r);
  return 0;
}
