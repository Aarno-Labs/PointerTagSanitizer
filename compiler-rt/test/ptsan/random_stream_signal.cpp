//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clangxx_raw -std=c++17 -O2 -finstrument-functions -fsanitize=bounds -fsanitize-trap=bounds -I%S/../../lib %s -o %t
// RUN: %run %t
//
// The instrumentation exit hook delivers a signal after refill finishes but
// before nextU32 resumes. The handler consumes exactly one buffer. The original
// implementation then read Buffer[256] and left Index at 257. No test hooks are
// needed in the production draw path.
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SANITIZER_COMMON_NO_REDEFINE_BUILTINS
#include "ptsan/ptsan.h"
// Inspect the cursor and refill counter without adding a runtime interface.
#define private public
#include "ptsan/ptsan_random.h"
#undef private
#include "ptsan/ptsan_random.cpp"

unsigned ClRandomSeed = 1;
namespace __sanitizer {
bool GetRandom(void *, uptr, bool) { abort(); }
void Printf(const char *, ...) { abort(); }
void internal__exit(int Code) { exit(Code); }
} // namespace __sanitizer

static __ptsan::RandomStream Stream;
static volatile sig_atomic_t Pending, Depth, Handled;
static volatile u32 Sink;

// Bind the hook to the non-inlined member function's entry address.
extern "C" void refillEntry() asm("_ZN7__ptsan12RandomStream6refillEv");

extern "C" __attribute__((no_instrument_function)) void
__cyg_profile_func_enter(void *, void *) {}

extern "C" __attribute__((no_instrument_function)) void
__cyg_profile_func_exit(void *Function, void *) {
  if (Function != reinterpret_cast<void *>(&refillEntry))
    return;
  sig_atomic_t Bit = 1 << Depth;
  if (Pending & Bit) {
    Pending &= ~Bit;
    assert(raise(SIGUSR1) == 0);
  }
}

static void handler(int Signal) {
  assert(Signal == SIGUSR1);
  ++Depth;
  for (unsigned I = 0; I != Stream.BufferWords; ++I) {
    Sink = Stream.nextU32();
    assert(__atomic_load_n(&Stream.Index, __ATOMIC_RELAXED) <=
           Stream.BufferWords);
  }
  --Depth;
  ++Handled;
}

static void exercise(unsigned Levels) {
  Stream.initialize();
  Handled = 0;
  Pending = (1 << Levels) - 1;
  Sink = Stream.nextU32();
  assert(Depth == 0 && Pending == 0);
  assert(Handled == (sig_atomic_t)Levels);
  assert(__atomic_load_n(&Stream.Index, __ATOMIC_RELAXED) == 1);

  // The interrupted draw must not poison later draws or prevent new refills.
  u64 Counter = __atomic_load_n(&Stream.Counter, __ATOMIC_RELAXED);
  for (unsigned I = 0; I != 3 * Stream.BufferWords; ++I) {
    Sink = Stream.nextU32();
    assert(__atomic_load_n(&Stream.Index, __ATOMIC_RELAXED) <=
           Stream.BufferWords);
  }
  assert(__atomic_load_n(&Stream.Index, __ATOMIC_RELAXED) == 1);
  assert(__atomic_load_n(&Stream.Counter, __ATOMIC_RELAXED) ==
         Counter + 3 * __ptsan::RandomBatchBlocks);
}

int main() {
  struct sigaction Action = {};
  Action.sa_handler = handler;
  Action.sa_flags = SA_NODEFER;
  assert(sigemptyset(&Action.sa_mask) == 0);
  assert(sigaction(SIGUSR1, &Action, nullptr) == 0);
  for (unsigned I = 0; I != 8; ++I) {
    exercise(1);
    exercise(4);
  }
  puts("signal and nested-signal draws retain bounded cursors and refill");
}
