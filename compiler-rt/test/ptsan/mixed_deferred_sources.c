//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -o %t %s
// RUN: %t

// One ID batch contains fixed storage, a dynamic alloca, and varargs backing
// storage. Reordering the batch must preserve each source's ID and bounds.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

static volatile unsigned size = 37;

__attribute__((noinline)) static void consume(va_list args, char *fixed,
                                             char *dynamic) {
  uintptr_t fixed_id = (uintptr_t)fixed >> 48;
  uintptr_t dynamic_id = (uintptr_t)dynamic >> 48;
  assert(fixed_id && fixed_id != 0xffff);
  assert(dynamic_id && dynamic_id != 0xffff);
  assert(fixed_id != dynamic_id);
  unsigned last = va_arg(args, unsigned);
  fixed[15] = 41;
  dynamic[last] = 42;
}

__attribute__((noinline)) static void mixed(unsigned n, ...) {
  char dynamic[n];
  char fixed[16];
  va_list args;
  va_start(args, n);
  consume(args, fixed, dynamic);
  va_end(args);
  assert(fixed[15] == 41);
  assert(dynamic[n - 1] == 42);
}

int main(void) {
  for (unsigned i = 0; i < 1000; ++i) {
    unsigned n = size + i % 13;
    mixed(n, n - 1);
  }
  return 0;
}
