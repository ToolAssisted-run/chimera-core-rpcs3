// The scheduler's "aside" (waterbox/vsched.cpp): work during which the
// machine stands still. Built from vsched.cpp alone, no emulator.
//
// Four kinds of thread, as in the machine:
//  - a CPU of the console, which runs guest instructions and charges for them;
//  - a timer, which sleeps a millisecond at a time;
//  - a helper, which waits to be asked for something and does it;
//  - the "renderer", which does a piece of work that asks the helper, sleeps,
//    makes a thread and waits for it, and yields a hundred times.
//
// The renderer does that work three times:
//  1. plainly. The CPU runs beside it, the timer ticks and the clock moves.
//     This is the control: it shows the test can see the machine move.
//  2. set aside. The CPU does not run, the timer does not tick, the clock is
//     where it was - and the work was done all the same, sleep included.
//  3. set aside, waiting for something only the CPU can do. The aside has to
//     be given up, and the work finishes with the machine running again.
//
// Prints one line a step and exits 0 when all three are as they should be.
// SPDX-License-Identifier: MIT
#include "vsched.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t g_cpu_turns, g_ticks, g_stop;
static uint32_t g_helper_asked, g_helper_done;
static uint32_t g_made;
static uint32_t g_cpu_asked, g_cpu_answered;
static uint32_t g_finished;
static int g_failures;

static void wait_change(const uint32_t* word, uint32_t old)
{
  while (*word == old)
  {
    vsched_wait_item it = {word, old};
    vsched_wait(&it, 1, VSCHED_INFINITE);
  }
}

static void* cpu_fn(void*)
{
  while (!g_stop)
  {
    g_cpu_turns++;
    if (g_cpu_asked != g_cpu_answered)
    {
      g_cpu_answered = g_cpu_asked;
      vsched_notify(&g_cpu_answered, 1);
    }
    vsched_budget_expired(VSCHED_PPU);
  }
  return nullptr;
}

static void* timer_fn(void*)
{
  while (!g_stop)
  {
    vsched_sleep(1000000);
    g_ticks++;
  }
  return nullptr;
}

static void* helper_fn(void*)
{
  uint32_t seen = 0;
  for (;;)
  {
    wait_change(&g_helper_asked, seen);
    seen = g_helper_asked;
    g_helper_done++;
    vsched_notify(&g_helper_done, 1);
  }
  return nullptr;
}

static void* made_fn(void*)
{
  g_made++;
  vsched_notify(&g_made, 1);
  return nullptr;
}

struct seen_t
{
  uint32_t cpu_turns, ticks;
  uint64_t machine_ns, unseen_ns;
};

static seen_t work(bool aside)
{
  const uint32_t cpu0 = g_cpu_turns, ticks0 = g_ticks;
  const uint64_t now0 = vsched_now_ns();
  if (aside)
    vsched_aside_begin();

  const uint32_t done = g_helper_done;
  g_helper_asked++;
  vsched_notify(&g_helper_asked, 1);
  wait_change(&g_helper_done, done);

  vsched_sleep(5000000);

  const uint32_t made = g_made;
  if (vsched_create(nullptr, made_fn, nullptr) != 0)
    abort();
  wait_change(&g_made, made);

  for (int i = 0; i < 100; i++)
    vsched_yield_default();

  seen_t s;
  s.unseen_ns = aside ? vsched_aside_end() : 0;
  s.cpu_turns = g_cpu_turns - cpu0;
  s.ticks = g_ticks - ticks0;
  s.machine_ns = vsched_now_ns() - now0;
  return s;
}

static void check(bool ok, const char* what)
{
  printf("%s: %s\n", ok ? "ok" : "WRONG", what);
  if (!ok)
    g_failures++;
}

static void* renderer_fn(void*)
{
  // let the CPU charge a few quanta and the timer tick first
  vsched_sleep(3000000);

  const seen_t plain = work(false);
  printf("plain: the CPU ran %u times, the timer ticked %u times, the clock moved %llu us\n",
         plain.cpu_turns, plain.ticks, (unsigned long long)(plain.machine_ns / 1000));
  check(plain.cpu_turns > 0 && plain.ticks >= 4 && plain.machine_ns >= 5000000,
        "without an aside the machine moves beside the work (the control)");

  const seen_t aside = work(true);
  printf("aside: the CPU ran %u times, the timer ticked %u times, the clock moved %llu us; the work took %llu us on its own clock\n",
         aside.cpu_turns, aside.ticks, (unsigned long long)(aside.machine_ns / 1000), (unsigned long long)(aside.unseen_ns / 1000));
  check(aside.cpu_turns == 0, "set aside, no CPU of the console ran");
  check(aside.ticks == 0, "set aside, no wait that began before it timed out");
  check(aside.machine_ns == 0, "set aside, the machine's clock is where it was");
  check(aside.unseen_ns >= 5000000, "set aside, the work's own sleep ended on its own clock");

  // after it the machine goes on as before
  const uint32_t ticks0 = g_ticks;
  vsched_sleep(3000000);
  check(g_ticks - ticks0 >= 2, "after the aside the timer ticks again");

  const uint64_t given_up = vsched_aside_given_up();
  vsched_aside_begin();
  const uint32_t answered = g_cpu_answered;
  g_cpu_asked++;
  wait_change(&g_cpu_answered, answered);
  const uint64_t unseen = vsched_aside_end();
  check(vsched_aside_given_up() == given_up + 1 && unseen == 0,
        "work that needs a CPU of the console gives the aside up and is done all the same");

  g_finished = 1;
  vsched_notify(&g_finished, 1);
  return nullptr;
}

int main()
{
  vsched_init();
  if (vsched_create(nullptr, cpu_fn, nullptr) != 0 || vsched_create(nullptr, timer_fn, nullptr) != 0 ||
      vsched_create(nullptr, helper_fn, nullptr) != 0 || vsched_create(nullptr, renderer_fn, nullptr) != 0)
    return 2;
  // the driver waits a frame at a time, as the core's does
  while (!g_finished)
  {
    vsched_wait_item it = {&g_finished, 0};
    vsched_wait(&it, 1, 16666667);
  }
  fflush(stdout);
  // the other threads are parked for ever: leave without them
  _Exit(g_failures ? 1 : 0);
}
