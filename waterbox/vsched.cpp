// vsched: see vsched.h. The machine's own scheduler, host-agnostic.
// SPDX-License-Identifier: MIT
#include "vsched.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

// The costs that make virtual time out of work. These constants are the
// machine: a PPU instruction at IPC 0.5 on a 3.2 GHz core, an SPU
// instruction at IPC 1, an RSX FIFO command at 20 ns, a yield at 1 us.
// Movies cite the core version, and the core version pins them.
static const uint64_t PPU_INSTR_NS_Q10 = 640;   // 0.625 ns in 1/1024 ns
static const uint64_t SPU_INSTR_NS_Q10 = 320;   // 0.3125 ns
static const uint64_t RSX_CMD_NS_Q10 = 20480;   // 20 ns
static const int64_t BUDGET_PER_SLICE = 80000;  // instructions before a thread gives way
static const uint32_t RSX_CMDS_PER_SLICE = 2500;  // 50 us of FIFO work
static const uint64_t YIELD_COST_NS = 1000;
static const size_t THREAD_STACK_BYTES = 4u << 20;

enum state_t
{
  RUNNABLE,
  WAITING,
  DEAD,
};

struct vthread
{
  sem_t sem;
  pthread_t pt;
  uint32_t id;
  state_t state;
  uint64_t deadline;  // VSCHED_INFINITE when not timed
  uint64_t wait_seq;  // FIFO order among waiters
  const vsched_wait_item* items;
  int nitems;
  int timed_out;
  uint32_t mxcsr;
  uint16_t fcw;
  vsched_entry_fn fn;
  void* arg;
  int slot;
  vthread* next;  // creation-order ring
};



static vthread* g_head;  // first created (the driver)
static vthread* g_tail;
static vthread* g_cur;
static uint64_t g_now;
static uint32_t g_next_id;
static uint64_t g_wait_seq;
static uint64_t g_switches;
static int g_count;

int64_t vsched_budget = BUDGET_PER_SLICE;

[[noreturn]] static void die(const char* what)
{
  fprintf(stderr, "vsched: %s (now=%llu ns, threads=%d)\n", what, (unsigned long long)g_now, g_count);
  for (vthread* t = g_head; t; t = t->next)
    fprintf(stderr, "  thread %u: %s deadline=%llu items=%d\n", t->id,
            t->state == RUNNABLE ? "runnable" : t->state == WAITING ? "waiting" : "dead",
            (unsigned long long)t->deadline, t->nitems);
  fflush(stderr);
  abort();
}

static bool g_slot_used[VSCHED_SLOTS];
static void (*g_slot_resets[1024])(int);
static int g_slot_reset_count;
static void (*g_thread_exits[64])(int);
static int g_thread_exit_count;

static int take_slot(void)
{
  for (int i = 0; i < VSCHED_SLOTS; i++)
    if (!g_slot_used[i])
    {
      g_slot_used[i] = true;
      for (int r = 0; r < g_slot_reset_count; r++)
        g_slot_resets[r](i);
      return i;
    }
  die("out of thread slots");
}

static void fpu_save(vthread* t)
{
  t->mxcsr = _mm_getcsr();
  __asm__ volatile("fnstcw %0" : "=m"(t->fcw));
}

static void fpu_restore(const vthread* t)
{
  _mm_setcsr(t->mxcsr);
  __asm__ volatile("fldcw %0" : : "m"(t->fcw));
}

// Wake every timed waiter whose deadline has passed.
static void wake_expired(void)
{
  for (vthread* t = g_head; t; t = t->next)
    if (t->state == WAITING && t->deadline <= g_now)
    {
      t->state = RUNNABLE;
      t->timed_out = 1;
    }
}

// The next runnable thread after `from` in ring order, or null.
static vthread* next_runnable_after(vthread* from)
{
  vthread* t = from->next ? from->next : g_head;
  for (int i = 0; i < g_count; i++)
  {
    if (t->state == RUNNABLE)
      return t;
    t = t->next ? t->next : g_head;
  }
  return nullptr;
}

// When nothing is runnable, time jumps to the earliest deadline.
static void advance_to_next_deadline(void)
{
  uint64_t best = VSCHED_INFINITE;
  for (vthread* t = g_head; t; t = t->next)
    if (t->state == WAITING && t->deadline < best)
      best = t->deadline;
  if (best == VSCHED_INFINITE)
    die("every thread is waiting forever");
  if (best > g_now)
    g_now = best;
  wake_expired();
}

// Hand the machine to `t` and park until scheduled again.
static void switch_to(vthread* t)
{
  vthread* self = g_cur;
  if (t == self)
    return;
  fpu_save(self);
  g_cur = t;
  g_switches++;
  vsched_budget = BUDGET_PER_SLICE;
  sem_post(&t->sem);
  while (sem_wait(&self->sem) != 0)
  {
  }
  if (g_cur != self)
    die("the baton was duplicated: a thread resumed without being scheduled");
  fpu_restore(self);
}

// The current thread is no longer runnable: find who runs next.
static void schedule_away(void)
{
  vthread* self = g_cur;
  for (;;)
  {
    wake_expired();
    vthread* t = next_runnable_after(self);
    if (t)
    {
      switch_to(t);
      return;
    }
    if (self->state == RUNNABLE)
      return;  // only we can run: keep running
    advance_to_next_deadline();
  }
}

static void* trampoline(void* p)
{
  vthread* t = (vthread*)p;
  while (sem_wait(&t->sem) != 0)
  {
  }
  if (g_cur != t)
    die("the baton was duplicated: a new thread started without being scheduled");
  fpu_restore(t);
  t->fn(t->arg);
  vsched_exit();
  return nullptr;
}

void vsched_init(void)
{
  vthread* t = (vthread*)calloc(1, sizeof(vthread));
  sem_init(&t->sem, 0, 0);
  t->pt = pthread_self();
  t->id = g_next_id++;
  t->state = RUNNABLE;
  t->deadline = VSCHED_INFINITE;
  t->slot = take_slot();  // slot 0: its resetters already ran at registration
  fpu_save(t);
  g_head = g_tail = g_cur = t;
  g_count = 1;
  // the clock starts at one second: the emulator treats a zero time as
  // "not yet" in several places
  g_now = 1000000000ull;
}

// The RSX thread charges one FIFO command and gives way every so often.
void vsched_rsx_step(void)
{
  static uint32_t commands;
  g_now += RSX_CMD_NS_Q10 >> 10;
  if (++commands >= RSX_CMDS_PER_SLICE)
  {
    commands = 0;
    vsched_yield(0);
  }
}

int vsched_create(unsigned long* out_pthread, vsched_entry_fn fn, void* arg)
{
  vthread* t = (vthread*)calloc(1, sizeof(vthread));
  sem_init(&t->sem, 0, 0);
  t->id = g_next_id++;
  t->state = RUNNABLE;
  t->deadline = VSCHED_INFINITE;
  t->fn = fn;
  t->arg = arg;
  t->slot = take_slot();
  // a new thread starts with the machine's default FPU state, not the
  // creator's (rpcs3 sets rounding per thread anyway)
  t->mxcsr = 0x1f80;
  t->fcw = 0x37f;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES);
  int rc = pthread_create(&t->pt, &attr, trampoline, t);
  pthread_attr_destroy(&attr);
  if (rc != 0)
  {
    g_slot_used[t->slot] = false;
    sem_destroy(&t->sem);
    free(t);
    return rc;
  }
  g_tail->next = t;
  g_tail = t;
  g_count++;
  if (out_pthread)
    memcpy(out_pthread, &t->pt, sizeof(pthread_t));
  return 0;
}

void vsched_exit(void)
{
  vthread* self = g_cur;
  if (self == g_head)
    die("the driver thread tried to exit");
  for (int i = 0; i < g_thread_exit_count; i++)
    g_thread_exits[i](self->slot);
  self->state = DEAD;
  g_slot_used[self->slot] = false;
  // unlink
  vthread* prev = g_head;
  while (prev->next != self)
    prev = prev->next;
  prev->next = self->next;
  if (g_tail == self)
    g_tail = prev;
  g_count--;
  // choose the successor the way schedule_away would, starting after prev
  vthread* t = nullptr;
  for (;;)
  {
    wake_expired();
    t = next_runnable_after(prev);
    if (t)
      break;
    advance_to_next_deadline();
  }
  g_cur = t;
  g_switches++;
  vsched_budget = BUDGET_PER_SLICE;
  sem_post(&t->sem);
  // the thread struct outlives us until nobody can name it; leak it
  // deliberately (a few hundred bytes per thread ever created)
  pthread_exit(nullptr);
}

uint64_t vsched_now_ns(void)
{
  return g_now;
}

void vsched_advance(uint64_t ns)
{
  g_now += ns;
}

void vsched_yield(uint64_t cost_ns)
{
  if (!g_cur)
    return;  // before the machine exists (static initialisers), a yield is nothing
  g_now += cost_ns;
  wake_expired();
  vthread* t = next_runnable_after(g_cur);
  if (t)
    switch_to(t);
}

int vsched_wait(const vsched_wait_item* items, int n, uint64_t timeout_ns)
{
  if (!g_cur)
    return 0;  // static initialisers may not park: nobody could wake them
  for (int i = 0; i < n; i++)
    if (*(const volatile uint32_t*)items[i].addr != items[i].old)
      return 0;
  vthread* self = g_cur;
  self->items = items;
  self->nitems = n;
  self->timed_out = 0;
  self->wait_seq = ++g_wait_seq;
  self->deadline = timeout_ns == VSCHED_INFINITE ? VSCHED_INFINITE : g_now + timeout_ns;
  self->state = WAITING;
  schedule_away();
  self->state = RUNNABLE;
  self->items = nullptr;
  self->nitems = 0;
  self->deadline = VSCHED_INFINITE;
  return self->timed_out;
}

void vsched_notify(const void* addr, int all)
{
  for (;;)
  {
    vthread* best = nullptr;
    for (vthread* t = g_head; t; t = t->next)
    {
      if (t->state != WAITING)
        continue;
      for (int i = 0; i < t->nitems; i++)
        if (t->items[i].addr == addr)
        {
          if (!best || t->wait_seq < best->wait_seq)
            best = t;
          break;
        }
    }
    if (!best)
      return;
    best->state = RUNNABLE;
    best->timed_out = 0;
    if (!all)
      return;
  }
}

void vsched_sleep(uint64_t ns)
{
  static uint32_t nobody;
  vsched_wait_item it = {&nobody, 0};
  vsched_wait(&it, 1, ns);
}

void vsched_budget_expired(int kind)
{
  uint64_t q10 = kind == VSCHED_PPU ? PPU_INSTR_NS_Q10 : kind == VSCHED_SPU ? SPU_INSTR_NS_Q10 : RSX_CMD_NS_Q10;
  uint64_t cost = ((uint64_t)BUDGET_PER_SLICE * q10) >> 10;
  vsched_budget = BUDGET_PER_SLICE;
  vsched_yield(cost);
}

int vsched_slot(void)
{
  return g_cur ? g_cur->slot : 0;
}

void vsched_register_slot_reset(void (*fn)(int))
{
  if (g_slot_reset_count >= 1024)
    die("too many thread-local resetters");
  g_slot_resets[g_slot_reset_count++] = fn;
  fn(0);
}

void vsched_register_thread_exit(void (*fn)(int))
{
  if (g_thread_exit_count >= 64)
    die("too many thread-exit hooks");
  g_thread_exits[g_thread_exit_count++] = fn;
}

int vsched_thread_count(void)
{
  return g_count;
}

uint32_t vsched_current_id(void)
{
  return g_cur ? g_cur->id : 0;
}

uint64_t vsched_switch_count(void)
{
  return g_switches;
}

// A yield that costs the fixed yield quantum, for the emulator's own
// spin-and-yield sites.
extern "C" void vsched_yield_default(void)
{
  vsched_yield(YIELD_COST_NS);
}
