//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-sanitize=ptsan -c %S/Inputs/signal_uninstrumented.c -o %t.o
// RUN: %ptsan_musl_strict %s %t.o -o %t
// RUN: not --crash %run %t 2>&1 | FileCheck %s
// CHECK: PTSan: signal handler has no compiler-generated entry
#include "Inputs/signal_test.h"

void uninstrumented_handler(int);

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = uninstrumented_handler;
  // Missing PTSan support must terminate, not become an application error.
  sigaction(SIGUSR1, &action, 0);
  return 0;
}
