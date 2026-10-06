//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -O2 \
// RUN:   -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t
// RUN: not %t before 2>&1 | FileCheck %s
// RUN: not %t after end 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-no-opts \
// RUN:   -mllvm -ptsan-abort-on-error -o %t.noopts %s
// RUN: %t.noopts
// RUN: not %t.noopts before 2>&1 | FileCheck %s
// RUN: not %t.noopts after end 2>&1 | FileCheck %s

// CHECK: Found bad

// Bounds must use the runtime element count times the element size, rather
// than the count alone or the stack allocation's alignment-rounded extent.
#include <assert.h>

static volatile unsigned count = 37;

__attribute__((noinline)) static void store(volatile int *p, int index) {
  p[index] = 123;
}

__attribute__((noinline)) static void exercise(int mode) {
  unsigned size = count;
  int buffer[size];
  store(buffer, 0);
  store(buffer, size - 1);
  assert(buffer[0] == 123 && buffer[size - 1] == 123);
  if (mode) {
    store(buffer, mode < 0 ? -1 : (int)size);
  }
}

int main(int argc, char **argv) {
  exercise(argc == 1 ? 0 : argc == 2 ? -1 : 1);
  return 0;
}
