//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-stack-temporal-safety=1 %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict_regular -mllvm -ptsan-stack-temporal-safety=1 %s -o %t.regular
// RUN: %run %t.regular

#include "Inputs/signal_test.h"
#include <sys/time.h>

extern __thread uint32_t *ptsan_stack_origin_next;
static volatile sig_atomic_t interruptions;

static SIGNAL_RAW uint32_t *cursor(void) { return ptsan_stack_origin_next; }

__attribute__((noinline)) static void frame(void) {
  char local[17];
  signal_touch(local, sizeof(local));
  signal_check_live(local, sizeof(local));
}

static void handler(int signo) {
  char local[17];
  signal_touch(local, sizeof(local));
  signal_check_live(local, sizeof(local));
  SIGNAL_CHECK(signal_in_signal_context());
  uint32_t *before = cursor();
  // Exercise generated slow-ID origin draws, including nested signal entry.
  // Even more frames than fit in the cache must leave its cursor untouched.
  for (unsigned i = 0; i != 1100; ++i)
    frame();
  if (signo == SIGUSR1)
    SIGNAL_CHECK(raise(SIGUSR2) == 0);
  SIGNAL_CHECK(cursor() == before);
  signal_check_bytes(local, sizeof(local));
  ++interruptions;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = handler;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR2, &action, 0) == 0);
  SIGNAL_CHECK(sigaction(SIGALRM, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(interruptions == 2);
  struct itimerval timer = {{0, 1000}, {0, 1000}};
  SIGNAL_CHECK(setitimer(ITIMER_REAL, &timer, 0) == 0);
  // Exercise interruptions during inline draws and cold refills.
  for (unsigned i = 0; i != 200000; ++i)
    frame();
  timer.it_value.tv_usec = timer.it_interval.tv_usec = 0;
  SIGNAL_CHECK(setitimer(ITIMER_REAL, &timer, 0) == 0);
  SIGNAL_CHECK(interruptions > 2);
  frame();
  return 0;
}
