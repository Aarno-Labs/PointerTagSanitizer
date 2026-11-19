//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// In-range integer arithmetic preserves packed pointers across storage and
// calls. General integer arithmetic that overflows the payload is unsupported;
// it does not go through the pointer repacking overflow check.
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -I%S %t.dir/address.c %t.dir/main.c -o %t
// RUN: %run %t
// RUN: %clang_strip -O2 -c %t.dir/address.c -o %t.dir/address.o
// RUN: %clang_ptsan -O2 -I%S %t.dir/main.c %t.dir/address.o -o %t.strip
// RUN: %run %t.strip

// RUN: %clang_ptsan_id32 -O2 -I%S %t.dir/address.c %t.dir/main.c -o %t
// RUN: %run %t
// RUN: %clang_strip -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -c %t.dir/address.c -o %t.dir/address.o
// RUN: %clang_ptsan_id32 -O2 -I%S %t.dir/main.c %t.dir/address.o -o %t.strip
// RUN: %run %t.strip

//--- address.c
#include <stdint.h>

volatile uintptr_t saved;

uintptr_t advance(char *p, uintptr_t n) {
  uintptr_t address = (uintptr_t)p + n;
  saved = address;
  return address;
}

uintptr_t retreat(char *p, uintptr_t n) {
  return (uintptr_t)p - n;
}

char *as_pointer(char *p, uintptr_t n) {
  return (char *)((uintptr_t)p + n);
}

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

extern volatile uintptr_t saved;
uintptr_t advance(char *, uintptr_t);
uintptr_t retreat(char *, uintptr_t);
char *as_pointer(char *, uintptr_t);

int main(void) {
  char *p = malloc(512);
  assert(p && get_id(p));
  uintptr_t bits = advance(p, 128);
  char *q = (char *)bits;
  assert(saved == bits);
  assert(get_id(q) == get_id(p));
  assert(ptsan_get_address(q) == ptsan_get_address(p) + 128);
  *q = 42;
  assert(p[128] == 42);
  q = (char *)retreat(p + 256, 128);
  assert(get_id(q) == get_id(p));
  assert(*q == 42);
  q = as_pointer(p, 128);
  assert(get_id(q) == get_id(p));
  assert(*q == 42);
  free(p);
}
