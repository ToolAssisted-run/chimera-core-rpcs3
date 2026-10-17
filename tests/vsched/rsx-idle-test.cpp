// The RSX's queue (waterbox/vsched.cpp, vsched_rsx_idle). Built from
// vsched.cpp alone, no emulator.
//
// Two threads, as in the machine:
//  - a CPU of the console, which runs guest instructions and charges for them
//    a budget at a time, and once gives the RSX commands by moving the queue's
//    write pointer in the middle of its slice;
//  - the RSX, which takes the commands it was given and, with nothing left,
//    waits for more with vsched_rsx_idle.
//
// What must hold:
//  1. With nothing given, the CPU runs its whole slice, 20 budgets (80,000
//     instructions in budgets of 4,000), between two turns of the RSX. This is
//     the control: it shows that the RSX does not simply run after every
//     budget.
//  2. Commands given in the middle of a slice are taken at the end of that
//     same budget, before the CPU runs its next one.
//  3. The clock is charged as it was: a slice of the CPU and one turn of the
//     idle RSX move it 50 us and 1 us.
//
// Prints one line a check and exits 0 when all hold.
// SPDX-License-Identifier: MIT
#include "vsched.h"

#include <stdio.h>
#include <stdlib.h>

static const uint32_t BUDGETS_PER_SLICE = 20;  // 80,000 / 4,000
static const uint32_t GIVE_AT = 47;            // the 7th budget of the 3rd slice
static const uint32_t TURNS = 8;

static uint32_t g_put, g_get;  // the queue's write and read pointers
static uint32_t g_budgets;     // budgets the CPU has run
static uint32_t g_given_at, g_taken_at;
static uint32_t g_turns;  // turns of the idle RSX
static uint32_t g_turn_budgets[TURNS];
static uint64_t g_turn_ns[TURNS];
static uint32_t g_finished;
static int g_failures;

static void check(bool ok, const char* what)
{
  printf("%s: %s\n", ok ? "ok" : "WRONG", what);
  if (!ok)
    g_failures++;
}

static void* rsx_fn(void*)
{
  for (;;)
  {
    if (g_put != g_get)
    {
      g_get = g_put;
      g_taken_at = g_budgets;
      continue;
    }
    if (g_turns < TURNS)
    {
      g_turn_budgets[g_turns] = g_budgets;
      g_turn_ns[g_turns] = vsched_now_ns();
    }
    g_turns++;
    vsched_rsx_idle(&g_put);
  }
  return nullptr;
}

static void* cpu_fn(void*)
{
  while (g_turns < TURNS)
  {
    g_budgets++;
    if (g_budgets == GIVE_AT)
    {
      g_put++;
      g_given_at = g_budgets;
    }
    vsched_budget_expired(VSCHED_PPU);
  }

  printf("the idle RSX ran after budgets");
  for (uint32_t i = 0; i < TURNS; i++)
    printf(" %u", g_turn_budgets[i]);
  printf("; commands given in budget %u were taken in budget %u\n", g_given_at, g_taken_at);
  check(g_turn_budgets[1] - g_turn_budgets[0] == BUDGETS_PER_SLICE &&
            g_turn_budgets[2] - g_turn_budgets[1] == BUDGETS_PER_SLICE,
        "with nothing given, the CPU runs its whole slice of 20 budgets between two turns of the RSX (the control)");
  check(g_taken_at == g_given_at, "commands given in the middle of a slice are taken before the CPU's next budget");
  check(g_turn_budgets[4] - g_turn_budgets[3] == BUDGETS_PER_SLICE,
        "after that, the CPU runs whole slices again");
  check(g_turn_ns[2] - g_turn_ns[1] == 51000, "a slice of the CPU and a turn of the idle RSX cost 50 us and 1 us, as before");

  g_finished = 1;
  vsched_notify(&g_finished, 1);
  // park for ever; the driver ends the program
  static uint32_t never;
  for (;;)
  {
    vsched_wait_item it = {&never, 0};
    vsched_wait(&it, 1, VSCHED_INFINITE);
  }
  return nullptr;
}

int main()
{
  vsched_init();
  if (vsched_create(nullptr, rsx_fn, nullptr) != 0 || vsched_create(nullptr, cpu_fn, nullptr) != 0)
    return 2;
  while (!g_finished)
  {
    vsched_wait_item it = {&g_finished, 0};
    vsched_wait(&it, 1, VSCHED_INFINITE);
  }
  fflush(stdout);
  // the other threads are parked for ever: leave without them
  _Exit(g_failures ? 1 : 0);
}
