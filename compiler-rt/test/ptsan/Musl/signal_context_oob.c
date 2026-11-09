//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: not %run %t i 2>&1 | FileCheck %s
// RUN: not %run %t c 2>&1 | FileCheck %s
// RUN: not %run %t s 2>&1 | FileCheck %s
// RUN: not %run %t u 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: not %run %t.id32 i 2>&1 | FileCheck %s
// RUN: not %run %t.id32 c 2>&1 | FileCheck %s
// RUN: not %run %t.id32 s 2>&1 | FileCheck %s
// RUN: not %run %t.id32 u 2>&1 | FileCheck %s
// CHECK: CONTEXT-ACCESS-READY
// CHECK: Found bad
//
// Reject one-past accesses to each object, and escaped aliases after return.
// Require valid input metadata before printing READY, so an ID-zero failure
// at handler entry cannot accidentally satisfy a negative test.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <ucontext.h>

static char mode;
static const char *escaped;
static volatile unsigned char sink;

__attribute__((noinline)) static void read_byte(const char *p, size_t offset) {
  sink = p[offset];
}

static void ready(void) {
  static const char text[] = "CONTEXT-ACCESS-READY\n";
  SIGNAL_CHECK(write(2, text, sizeof(text) - 1) == sizeof(text) - 1);
}

static void handler(int signo, siginfo_t *info, void *context) {
  SIGNAL_CHECK(signo == SIGUSR1);
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  SIGNAL_CHECK(info->si_signo == signo);
  if (mode == 's' || mode == 'u') {
    escaped = mode == 's' ? (const char *)info : context;
    return;
  }
  ready();
  if (mode == 'i') {
    read_byte((const char *)info, sizeof(*info));
  } else {
    read_byte(context, sizeof(ucontext_t));
  }
}

int main(int argc, char **argv) {
  SIGNAL_CHECK(argc == 2);
  mode = argv[1][0];
  struct sigaction action = {0};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  if (mode == 's' || mode == 'u') {
    ready();
    read_byte(escaped, 0);
  }
  return 0;
}
