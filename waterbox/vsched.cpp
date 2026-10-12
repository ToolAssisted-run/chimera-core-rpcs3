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
  // see "aside" below
  bool cpu;         // has run guest instructions: one of the console's CPUs
  bool held;        // does not run until the aside ends
  bool aside_wait;  // its wait began during the aside, on the aside's clock
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

int64_t vsched_budget_slice(void)
{
  return BUDGET_PER_SLICE;
}

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

// ---- aside: work the machine must not notice ------------------------------
//
// After a savestate is loaded the renderer makes all its objects again, on
// the RSX thread, in the middle of a frame. That work waits and yields like
// any other, so it used to cost the machine time and turns: the console's
// CPUs ran on while the RSX stood there, and the clock moved. A run that had
// been rewound was therefore not the run that had not - in Dead or Alive 5,
// 37,443 bytes of memory apart a hundred frames later, and one frame shown a
// frame late.
//
// Between vsched_aside_begin and vsched_aside_end the machine stands still:
//  - nobody who was about to run runs, and no CPU of the console runs at all;
//  - no wait that began before it times out;
//  - the clock the work sees keeps going, so that its own sleeps end, and is
//    put back where it was when the work is done.
// Who can run is the thread that asked, threads made during it, and threads
// that were waiting to be asked for something and are asked: the helpers a
// renderer hands work to.
//
// If the work turns out to need someone who is standing still - nothing can
// run and nothing is due, or it goes on past any sensible length - the aside
// is given up: everyone runs again, as they always did, and a line says so.
static vthread* g_aside;        // who asked; null when there is none
static uint64_t g_aside_then;   // the machine's time when it began
static uint64_t g_aside_switches;
static uint64_t g_aside_last_ns;
static uint64_t g_aside_given_up;
static const uint64_t ASIDE_LIMIT_NS = 120ull * 1000000000ull;
static const uint64_t ASIDE_LIMIT_SWITCHES = 200ull * 1000 * 1000;

static void aside_close(const char* gave_up)
{
  if (!g_aside)
    return;
  const uint64_t spent = g_now - g_aside_then;
  for (vthread* t = g_head; t; t = t->next)
  {
    // a wait still running on the aside's clock has the rest of its time
    // on the machine's
    if (t->state == WAITING && t->aside_wait && t->deadline != VSCHED_INFINITE)
      t->deadline = t->deadline > g_now ? g_aside_then + (t->deadline - g_now) : g_aside_then;
    t->held = false;
    t->aside_wait = false;
  }
  g_now = g_aside_then;
  g_aside_last_ns = spent;
  g_aside = nullptr;
  if (gave_up)
  {
    g_aside_given_up++;
    fprintf(stderr, "vsched: work set aside from the machine was given up after %llu us (%s); the machine runs on beside it\n",
            (unsigned long long)(spent / 1000), gave_up);
  }
}

void vsched_aside_begin(void)
{
  if (!g_cur || g_aside)
    return;
  g_aside = g_cur;
  g_aside_then = g_now;
  g_aside_switches = g_switches;
  for (vthread* t = g_head; t; t = t->next)
  {
    t->held = t != g_cur && (t->state == RUNNABLE || t->cpu);
    t->aside_wait = false;
  }
}

uint64_t vsched_aside_end(void)
{
  if (g_aside != g_cur)
    return 0;  // none, or given up, or not this thread's to end
  aside_close(nullptr);
  return g_aside_last_ns;
}

uint64_t vsched_aside_given_up(void)
{
  return g_aside_given_up;
}

// Wake every timed waiter whose deadline has passed.
static void wake_expired(void)
{
  if (g_aside && (g_now - g_aside_then > ASIDE_LIMIT_NS || g_switches - g_aside_switches > ASIDE_LIMIT_SWITCHES))
    aside_close("it went on too long");
  for (vthread* t = g_head; t; t = t->next)
    if (t->state == WAITING && t->deadline <= g_now && (!g_aside || t->aside_wait))
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
    if (t->state == RUNNABLE && !(g_aside && t->held))
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
    if (t->state == WAITING && t->deadline < best && (!g_aside || t->aside_wait))
      best = t->deadline;
  if (best == VSCHED_INFINITE && g_aside)
  {
    // the work needs someone who is standing still
    aside_close("it waited for a thread that was standing still");
    wake_expired();
    return;
  }
  if (best == VSCHED_INFINITE)
    die("every thread is waiting forever");
  if (best > g_now)
    g_now = best;
  wake_expired();
}

static void (*g_leave_hook)(void) = nullptr;

void vsched_set_leave_hook(void (*fn)(void))
{
  g_leave_hook = fn;
}

// Hand the machine to `t` and park until scheduled again.
static void switch_to(vthread* t)
{
  vthread* self = g_cur;
  if (t == self)
    return;
  if (g_leave_hook)
    g_leave_hook();
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
  self->aside_wait = g_aside != nullptr;
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
  if (g_cur && (kind == VSCHED_PPU || kind == VSCHED_SPU))
    g_cur->cpu = true;
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
