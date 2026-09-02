// vsched: the virtual-time scheduler that owns every one of the emulator's
// threads. One thread runs at a time; the running thread hands off
// explicitly; virtual time advances by what the machine charges (interpreter
// quanta, yields) and jumps to the next deadline when nothing is runnable.
// Plain pthreads + semaphores in both flavors, so the native build follows
// exactly the schedule the sandbox follows. No thread-local storage anywhere.
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VSCHED_INFINITE UINT64_MAX

typedef void* (*vsched_entry_fn)(void*);

typedef struct vsched_wait_item
{
  const void* addr;
  uint32_t old;
} vsched_wait_item;

// The calling thread becomes thread 0 (the driver). Call once, first.
void vsched_init(void);

// A pthread that starts parked and runs only when scheduled; threads are
// scheduled round robin in creation order. `out_pthread` receives the
// pthread_t so the caller's join still works.
int vsched_create(unsigned long* out_pthread, vsched_entry_fn fn, void* arg);

// The current thread leaves the machine (does not return).
void vsched_exit(void);

// Virtual time in nanoseconds since the machine started.
uint64_t vsched_now_ns(void);

// Charge virtual time without giving way.
void vsched_advance(uint64_t ns);

// Charge virtual time and give way to the next runnable thread, if any.
void vsched_yield(uint64_t cost_ns);

// Park until one of the addresses is notified, its value already differs
// from `old`, or the timeout elapses. Returns 1 on timeout, 0 otherwise;
// callers re-check their condition either way (futex semantics).
int vsched_wait(const vsched_wait_item* items, int n, uint64_t timeout_ns);

// Wake the longest-waiting thread (or every thread) parked on `addr`.
void vsched_notify(const void* addr, int all);

// Park for a duration of virtual time.
void vsched_sleep(uint64_t ns);

// Interpreter budgets: the interpreters decrement this per instruction and
// call vsched_budget_expired when it reaches zero; the kind decides the
// cost of the quantum just executed.
enum vsched_kind
{
  VSCHED_PPU = 0,
  VSCHED_SPU = 1,
  VSCHED_RSX = 2,
};
extern int64_t vsched_budget;
void vsched_budget_expired(int kind);

// The RSX thread charges one FIFO command per call and gives way every so
// often; the emulator's own spin-and-yield sites pay a fixed yield cost.
void vsched_rsx_step(void);
void vsched_yield_default(void);

// Thread-local storage without ELF TLS: every thread owns a slot in
// [0, VSCHED_SLOTS) for its whole life (slots are reused after exit), and a
// per-thread variable is an array indexed by the running thread's slot.
// Resetters registered here run for a slot whenever a thread is born into
// it (and once for slot 0 at registration), so each thread starts from the
// variable's initial value. See chimera_tls.h for the macros.
#define VSCHED_SLOTS 256
int vsched_slot(void);
void vsched_register_slot_reset(void (*fn)(int slot));
// Runs on the exiting thread, before its slot is freed (what a thread_local
// destructor used to do).
void vsched_register_thread_exit(void (*fn)(int slot));

// Diagnostics.
int vsched_thread_count(void);
uint32_t vsched_current_id(void);
uint64_t vsched_switch_count(void);

#ifdef __cplusplus
}
#endif
