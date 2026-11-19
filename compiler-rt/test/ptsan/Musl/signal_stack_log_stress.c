//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t setup
// RUN: %run %t cleanup
// RUN: %ptsan_musl_strict_regular -Wall -Wextra -Werror -DNO_STATS %s -o %t.regular
// RUN: %run %t.regular setup
// RUN: %run %t.regular cleanup
//
// Interrupt partially completed stack-ID allocation or cleanup with timer
// signals. Inside the handler, longjmp retires a separate mixed batch of slow
// stack IDs and repeatable-allocation IDs. The interrupted operation then
// resumes and must finish without losing IDs or consuming the handler's IDs.
// Jumps stay within the handler: abandoning a runtime operation is a separate,
// documented leak window, not what this test exercises.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <setjmp.h>
#include <sys/time.h>

enum { BatchSize = 2048, HandlerFixed = 32, HandlerTotal = 65 };
extern void ptsan_get_stack_ids(uint16_t *, uint64_t);
extern void ptsan_free_stack_ids(uintptr_t);
extern uint16_t ptsan_get_repeatable_alloca_id(void *, uint64_t, uint64_t);
extern struct {
  uint64_t base, span;
} ptsan_bounds[];

static sigjmp_buf inside_handler;
static volatile sig_atomic_t phase, interruptions;
static volatile uintptr_t batch_cursor;
static volatile uint16_t first_id, last_id;
static uint16_t abandoned[HandlerTotal];

static SIGNAL_RAW uintptr_t stack_cursor(void) {
  return ptsan_stack_id_log.cursor;
}

static SIGNAL_RAW uint64_t live_ids(void) {
#ifdef NO_STATS
  return 0;
#else
  return signal_live_stack_ids();
#endif
}

static SIGNAL_RAW __attribute__((noreturn)) void abandon_ids(sigjmp_buf env) {
  char objects[HandlerTotal];
  ptsan_get_stack_ids(abandoned, HandlerFixed);
  for (unsigned i = 0; i != HandlerFixed; ++i) {
    uint16_t id = abandoned[i];
    SIGNAL_CHECK(id && id != ptsan_default_id());
    // Fixed stack IDs get their bounds from the compiler; reproduce that here.
    ptsan_bounds[id].base = (uintptr_t)&objects[i] | ((uintptr_t)id << 48);
    ptsan_bounds[id].span = 1;
  }
  for (unsigned i = HandlerFixed; i != HandlerTotal; ++i) {
    abandoned[i] = ptsan_get_repeatable_alloca_id(&objects[i], 1, 1);
    SIGNAL_CHECK(abandoned[i] && abandoned[i] != ptsan_default_id());
  }
  // Receive the tagged environment from the instrumented caller. The raw
  // helper must not reconstruct an untagged address at the libc boundary.
  siglongjmp(env, 1);
}

static void handler(int signo) {
  SIGNAL_CHECK(signo == SIGALRM);
  if (!phase) {
    return;
  }
  uintptr_t cursor = stack_cursor();
  if (cursor <= batch_cursor || cursor >= batch_cursor + BatchSize) {
    return;
  }
  if (phase == 2 &&
      (ptsan_get_size(last_id) != 0 || ptsan_get_size(first_id) == 0)) {
    return;
  }

  uint64_t live = live_ids();
  if (sigsetjmp(inside_handler, 0) == 0) {
    // Keep IPA from replacing the raw helper's argument with the global
    // symbol, which would bypass tagging at this instrumented call site.
    void *env = inside_handler;
    __asm__ volatile("" : "+r"(env));
    abandon_ids(env);
  }
  SIGNAL_CHECK(stack_cursor() == cursor);
  SIGNAL_CHECK(live_ids() == live);
  for (unsigned i = 0; i != HandlerTotal; ++i) {
    SIGNAL_CHECK(ptsan_get_size(abandoned[i]) == 0);
  }
  ++interruptions;
}

static SIGNAL_RAW void exercise(int setup) {
  char objects[BatchSize];
  uintptr_t cursor = ptsan_stack_id_log.cursor;
  batch_cursor = cursor;
  phase = setup ? 1 : 0;
  for (unsigned i = 0; i != BatchSize; ++i) {
    uint16_t id = ptsan_get_repeatable_alloca_id(&objects[i], 1, 1);
    SIGNAL_CHECK(id && id != ptsan_default_id());
    if (i == 0) {
      first_id = id;
    }
    last_id = id;
  }
  phase = setup ? 0 : 2;
  ptsan_free_stack_ids(cursor);
  phase = 0;
  SIGNAL_CHECK(ptsan_stack_id_log.cursor == cursor);
}

int main(int argc, char **argv) {
  SIGNAL_CHECK(argc == 2);
  int setup = argv[1][0] == 's';
  struct sigaction action = {0};
  action.sa_handler = handler;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGALRM, &action, 0) == 0);
  struct itimerval timer = {{0, 100}, {0, 100}};
  uint64_t baseline = live_ids();
  SIGNAL_CHECK(setitimer(ITIMER_REAL, &timer, 0) == 0);
  for (unsigned iteration = 0; iteration != 256; ++iteration) {
    exercise(setup);
    SIGNAL_CHECK(live_ids() == baseline);
  }
  timer.it_value.tv_usec = timer.it_interval.tv_usec = 0;
  SIGNAL_CHECK(setitimer(ITIMER_REAL, &timer, 0) == 0);
  SIGNAL_CHECK(interruptions > 0);
  return 0;
}
