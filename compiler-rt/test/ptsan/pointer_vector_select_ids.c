//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -mavx2 -mllvm -force-vector-width=4 \
// RUN:   -mllvm -force-vector-interleave=1 -mllvm -ptsan-abort-on-error \
// RUN:   -o %t %s
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t bad 2>&1 | FileCheck %s --check-prefix=BAD

// GOOD-NOT: Found bad
// GOOD: pointer-vector-select-ids-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access

// Select independently between two allocation IDs in each pointer-vector
// lane. Pointer equality alone would miss lost/swapped IDs, so check the bits
// of every result as well as valid loads and an out-of-bounds load.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

enum { Count = 64 };

__attribute__((noinline)) static void choose(
    char **restrict output, char *const *restrict left,
    char *const *restrict right, const int *restrict choices,
    int *restrict different) {
#pragma clang loop vectorize(enable) interleave(disable)
  for (unsigned i = 0; i != Count; ++i) {
    different[i] = left[i] != right[i];
    output[i] = choices[i] ? left[i] : right[i];
  }
}

__attribute__((noinline)) static uintptr_t bits(const void *p) {
  return (uintptr_t)p;
}

__attribute__((noinline)) static char read_at(char *p, unsigned i) {
  return *(volatile char *)(p + i);
}

int main(int argc, char **argv) {
  char a[16] = {3}, b[16] = {5};
  char *left[Count], *right[Count], *output[Count];
  int choices[Count], different[Count];
  for (unsigned i = 0; i != Count; ++i) {
    left[i] = a;
    right[i] = b;
    choices[i] = i & 1;
  }
  assert(bits(a) >> 48 && bits(b) >> 48);
  assert(bits(a) >> 48 != bits(b) >> 48);
  choose(output, left, right, choices, different);
  for (unsigned i = 0; i != Count; ++i) {
    assert(different[i]);
    assert(bits(output[i]) == bits(choices[i] ? a : b));
    assert(read_at(output[i], 0) == (choices[i] ? 3 : 5));
  }
  if (argc > 1) {
    return read_at(output[3], 16);
  }
  puts("pointer-vector-select-ids-ok");
  return 0;
}
