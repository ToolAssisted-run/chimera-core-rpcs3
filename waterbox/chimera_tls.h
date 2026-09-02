// Thread-local variables without ELF TLS. The waterbox guest has no thread
// pointer, so `thread_local` cannot exist there; under vsched exactly one
// thread runs at a time and it knows its slot, so a per-thread variable is
// an array of VSCHED_SLOTS indexed by the running thread's slot. Both flavors
// use this, so the native reference exercises the same code.
//
//   thread_local T name = init;        ->  CHIMERA_TLS_DEFINE(T, name, init);
//                                          #define name CHIMERA_TLS(name)
//   static thread_local T name = init; ->  CHIMERA_TLS_DEFINE_STATIC(T, name, init);
//                                          #define name CHIMERA_TLS(name)
//   extern thread_local T name;        ->  CHIMERA_TLS_DECLARE(T, name);
//                                          #define name CHIMERA_TLS(name)
//   class member `static thread_local T name;`
//                                      ->  static T name##_slots[VSCHED_SLOTS];
//                                          (definition: CHIMERA_TLS_MEMBER(T, Class, name, init))
//                                          #define name CHIMERA_TLS(name)   (token pastes after :: too)
//
// The #define must come after the definition/declaration and be visible to
// every use; put shared ones in the header that used to declare the
// thread_local. A resetter runs for a slot each time a thread is born into
// it, so every thread starts from `init` (and slot 0 is initialised at
// static-init time, before vsched exists).
// SPDX-License-Identifier: MIT
#pragma once
#include "vsched.h"

struct chimera_tls_registrar
{
  chimera_tls_registrar(void (*fn)(int)) { vsched_register_slot_reset(fn); }
};

// no parentheses: the expansion must still follow a Class:: qualifier
#define CHIMERA_TLS(name) name##_slots[vsched_slot()]

#define CHIMERA_TLS_DEFINE(type, name, ...) \
  type name##_slots[VSCHED_SLOTS]{}; \
  static chimera_tls_registrar name##_tls_registrar([](int s) { name##_slots[s] = type(__VA_ARGS__); })

#define CHIMERA_TLS_DEFINE_STATIC(type, name, ...) \
  static type name##_slots[VSCHED_SLOTS]{}; \
  static chimera_tls_registrar name##_tls_registrar([](int s) { name##_slots[s] = type(__VA_ARGS__); })

#define CHIMERA_TLS_DECLARE(type, name) extern type name##_slots[VSCHED_SLOTS]

#define CHIMERA_TLS_MEMBER(type, cls, name, ...) \
  type cls::name##_slots[VSCHED_SLOTS]{}; \
  static chimera_tls_registrar cls##_##name##_tls_registrar([](int s) { cls::name##_slots[s] = type(__VA_ARGS__); })
