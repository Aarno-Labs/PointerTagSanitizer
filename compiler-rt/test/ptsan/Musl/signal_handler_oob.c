//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: not %run %t 2>&1 | FileCheck %s
// CHECK: HANDLER-OOB-READY
// CHECK: Found bad
//
// Keep handler instrumentation active: an ordinary local-buffer overrun must
// still be rejected when entered through the generated wrapper.
#include "Inputs/signal_test.h"

static volatile unsigned offset = 37;

__attribute__((noinline)) static void overwrite(char *p) { p[offset] = 1; }

static void handler(int signo) {
  char local[37];
  SIGNAL_CHECK(signo == SIGUSR1);
  signal_touch(local, sizeof(local));
  static const char ready[] = "HANDLER-OOB-READY\n";
  SIGNAL_CHECK(write(2, ready, sizeof(ready) - 1) == sizeof(ready) - 1);
  overwrite(local);
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = handler;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  return 0;
}
