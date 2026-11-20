//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict %s -o %t
// RUN: %run %t
//
// Leave exactly one ID for the SA_SIGINFO entry's two context objects. Bounds
// for the default-ID object must not overwrite the shared default metadata.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <ucontext.h>

extern void ptsan_get_stack_ids(uint16_t *, uint64_t);
extern void ptsan_free_stack_ids(uintptr_t);
extern void ptsan_signal_entry_3(int, siginfo_t *, void *,
                                 void (*)(int, siginfo_t *, void *));
// Reserve one ID outside the ownership log, then return it directly after
// exhausting both the thread allocation cache and shared pool.
extern uint16_t take_id_direct(void) __asm__("_ZN7__ptsan12takeIDDirectEv");
extern void return_id_direct(uint16_t) __asm__("_ZN7__ptsan14returnIDDirectEt");

static uintptr_t default_base, default_size;
static unsigned observed;

static SIGNAL_RAW void handler(int signo, siginfo_t *info, void *context) {
  SIGNAL_CHECK(signo == SIGUSR1);
  SIGNAL_CHECK(get_id(info) != 0 && get_id(info) != ptsan_default_id());
  SIGNAL_CHECK(ptsan_get_base(get_id(info)) == (uintptr_t)info);
  SIGNAL_CHECK(ptsan_get_size(get_id(info)) == sizeof(*info));
  SIGNAL_CHECK(get_id(context) == ptsan_default_id());
  SIGNAL_CHECK(ptsan_get_base(ptsan_default_id()) == default_base);
  SIGNAL_CHECK(ptsan_get_size(ptsan_default_id()) == default_size);
  ++observed;
}

SIGNAL_RAW int main(void) {
  uint16_t live[65536];
  siginfo_t info = {0};
  ucontext_t context = {0};
  uint64_t baseline = signal_live_stack_ids();
  default_base = ptsan_get_base(ptsan_default_id());
  default_size = ptsan_get_size(ptsan_default_id());
  uintptr_t cursor = ptsan_stack_id_log.cursor;
  uint16_t spare = take_id_direct();
  SIGNAL_CHECK(spare != 0);
  unsigned count = 0;
  while (count != 65536) {
    ptsan_get_stack_ids(&live[count], 1);
    if (live[count] == ptsan_default_id()) {
      break;
    }
    ++count;
  }
  SIGNAL_CHECK(count > 0 && count < 65536);
  return_id_direct(spare);

  // Call the same entry used by generated wrappers without libc consuming the
  // one remaining ID before entry. Real kernel delivery is tested separately.
  ptsan_signal_entry_3(SIGUSR1, &info, &context, handler);
  SIGNAL_CHECK(observed == 1);
  ptsan_free_stack_ids(cursor);
  SIGNAL_CHECK(signal_live_stack_ids() == baseline);
  return 0;
}
