//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-opts \
// RUN:   -mllvm -ptsan-abort-on-error -o %t.noopts %s
// RUN: %t.noopts 2>&1 | FileCheck %s

// CHECK-NOT: Found bad
// CHECK: global-constexpr-access-ok
// CHECK-NOT: Found bad

// A properly sized C counterpart of global_constexpr_lowering.ll: one address
// starts from a global-derived SSA value and the other from a constant GEP.
// Both must access the same raw storage while escaped pointers retain its ID.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct matrices {
  int header;
  float values[4][256];
} data;
static volatile unsigned column = 7;

__attribute__((noinline)) static unsigned id_of(const void *p) {
  return (uintptr_t)p >> 48;
}

__attribute__((noinline)) static void copy_column(unsigned i) {
  float (*source)[256] = data.values;
#pragma clang loop unroll(disable)
  for (unsigned row = 1; row != 3; ++row) {
    data.values[row][i] = source[row + 1][i];
  }
}

int main(void) {
  unsigned i = column;
  data.values[2][i] = 17;
  data.values[3][i] = 23;
  copy_column(i);
  assert(data.values[1][i] == 17 && data.values[2][i] == 23);
  assert(id_of(&data) && id_of(&data.values[2][i]) == id_of(&data));
  puts("global-constexpr-access-ok");
  return 0;
}
