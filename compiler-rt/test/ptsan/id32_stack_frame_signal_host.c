//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 %s -o %t
// RUN: %run %t

// Host libc does not wrap signal handlers. A handler can refill or exhaust
// the cache while generated code is consuming it. Repeated/skipped salts are
// acceptable; advancing beyond the sentinel is not.
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <sys/time.h>

#define RAW __attribute__((noinline, disable_sanitizer_instrumentation))
extern __thread uint32_t *ptsan_stack_origin_next;
extern uint32_t ptsan_refill_stack_origins(void);
static uint32_t *begin, *end;
static volatile sig_atomic_t handled;

static RAW void inspect(char *p) {
  assert(ptsan_get_origin(get_id(p)) >= 4096);
  assert(ptsan_get_size(get_id(p)) == 17);
}

__attribute__((noinline)) static void frame(void) {
  char local[17];
  inspect(local);
}

static RAW void handler(int signo) {
  assert(signo == SIGALRM);
  // Refill, then simulate consuming every word before the interrupted draw
  // resumes. Its cursor update must use the position it actually read.
  assert(ptsan_refill_stack_origins() != 0);
  ptsan_stack_origin_next = end;
  ++handled;
}

int RAW main(void) {
  assert(ptsan_refill_stack_origins() != 0);
  // The refill returns a separate salt, leaving the whole cache unconsumed.
  begin = ptsan_stack_origin_next;
  // Find the reserved zero sentinel without depending on the refill capacity.
  end = begin;
  while (*end)
    ++end;
  struct sigaction action = {0};
  action.sa_handler = handler;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGALRM, &action, 0) == 0);
  struct itimerval timer = {{0, 100}, {0, 100}};
  assert(setitimer(ITIMER_REAL, &timer, 0) == 0);
  for (unsigned i = 0; i != 1000000; ++i) {
    frame();
    assert(ptsan_stack_origin_next >= begin && ptsan_stack_origin_next <= end);
  }
  timer.it_value.tv_usec = timer.it_interval.tv_usec = 0;
  assert(setitimer(ITIMER_REAL, &timer, 0) == 0);
  assert(handled > 0);
  frame();
  return 0;
}
