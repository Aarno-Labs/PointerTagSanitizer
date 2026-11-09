//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Checks must interpret tagged-polarity inputs according to the decode policy:
// non-strict callers can supply raw ID-zero pointers. In particular, the low
// address check must not ignore ID zero's lower bound by using only the payload.
// Separate translation units keep the reader's input representation unknown.
// RUN: split-file %s %t
// RUN: %clang_ptsan_id32 -O2 -I %S/Inputs %t/main.c %t/read.c -o %t/test
// RUN: %run %t/test
// RUN: not %run %t/test low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %run %t/test indexed-low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %run %t/test before 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t/test end 2>&1 | FileCheck %s --check-prefix=BAD

// BAD: Found bad POINT access
// ZERO: Found bad POINT access: {{.*}}Ptr ID: 0x0

//--- main.c
#include "ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

int read_byte(volatile unsigned char *p);
int read_offset(volatile unsigned char *p, long delta);

int main(int argc, char **argv) {
  unsigned char *p = malloc(8);
  assert(p);
  p[0] = 17;
  p[7] = 23;
  assert(read_byte(p) == 17);
  assert(read_offset(p, 7) == 23);

#ifndef TEST_STRICT
  unsigned char *raw = (unsigned char *)(uintptr_t)ptsan_get_address(p);
  assert(read_byte(raw) == 17);
  assert(read_offset(raw, 7) == 23);
#endif

  if (argc > 1) {
    switch (argv[1][0]) {
    case 'l':
      return read_byte((unsigned char *)(uintptr_t)0x10);
    case 'i':
      return read_offset((unsigned char *)(uintptr_t)0x10, 0);
    case 'b':
      return read_byte(p - 1);
    case 'e':
      return read_byte(p + 8);
    }
    return 1;
  }
  free(p);
  return 0;
}

//--- read.c
__attribute__((noinline)) int read_byte(volatile unsigned char *p) {
  return *p;
}

__attribute__((noinline)) int read_offset(volatile unsigned char *p, long delta) {
  return p[delta];
}
