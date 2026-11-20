//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_strict -pthread -mllvm -ptsan-stack-temporal-safety=0 %s -o %t.weak
// RUN: %run %t.weak
//
// Wrappers preserve the interrupted code's fast-stack state while handlers use
// the signal-safe slow-ID cache.
#include "Inputs/signal_test.h"
#include <pthread.h>

static volatile sig_atomic_t calls;
static uint64_t expected_limit;
static uint64_t expected_next;

static void handler(int signo) {
  char local[37];
  SIGNAL_CHECK(signo == SIGUSR1);
  SIGNAL_CHECK(signal_in_signal_context() == 1);
  SIGNAL_CHECK(signal_stack_limit() == expected_limit);
  SIGNAL_CHECK(signal_next_stack_id() == expected_limit);
  signal_touch(local, sizeof(local));
  signal_check_bytes(local, sizeof(local));
  signal_check_live(local, sizeof(local));
  ++calls;
}

static void *exercise(void *unused) {
  (void)unused;
  uint64_t original = signal_stack_limit();
  SIGNAL_CHECK(original != 0);
  SIGNAL_CHECK(signal_in_signal_context() == 0);
  for (unsigned disabled = 0; disabled != 2; ++disabled) {
    expected_limit = disabled ? 0 : original;
    signal_set_stack_limit(expected_limit);
    for (int i = 0; i != 8; ++i) {
      expected_next = signal_next_stack_id();
      SIGNAL_CHECK(raise(SIGUSR1) == 0);
      SIGNAL_CHECK(signal_in_signal_context() == 0);
      SIGNAL_CHECK(signal_stack_limit() == expected_limit);
      // Instrumented musl's raise frame itself consumes one fast ID. With the
      // fast path disabled it uses the slow log, so the cursor is unchanged.
      if (expected_limit) {
        SIGNAL_CHECK(signal_next_stack_id() > expected_next);
        SIGNAL_CHECK(signal_next_stack_id() < expected_limit);
      } else {
        SIGNAL_CHECK(signal_next_stack_id() == expected_next);
      }
    }
  }
  signal_set_stack_limit(original);
  return 0;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = handler;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  exercise(0);
  pthread_t thread;
  SIGNAL_CHECK(pthread_create(&thread, 0, exercise, 0) == 0);
  SIGNAL_CHECK(pthread_join(thread, 0) == 0);
  SIGNAL_CHECK(calls == 32);
  return 0;
}
