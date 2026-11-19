//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 %s -o %t
// RUN: %run %t

// On x86-64 the 24-byte struct precedes a 16-byte-aligned long double on
// the stack. The caller lays out raw addresses, while va_arg rounds a tagged
// cursor. Both must choose the same padding.
#include <assert.h>
#include <stdarg.h>

struct Triple {
  long a, b, c;
};

__attribute__((noinline)) int read_arguments(int unused, ...) {
  va_list args;
  va_start(args, unused);
  struct Triple triple = va_arg(args, struct Triple);
  long double value = va_arg(args, long double);
  va_end(args);
  return triple.a == 11 && triple.b == 22 && triple.c == 33 && value == 4.5L;
}

int main(void) {
  for (unsigned i = 0; i < 16; ++i) {
    assert(read_arguments(0, (struct Triple){11, 22, 33}, 4.5L));
  }
  return 0;
}
