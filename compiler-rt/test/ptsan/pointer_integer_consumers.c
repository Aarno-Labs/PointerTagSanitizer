//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// Address arithmetic hides ptrtoint from the immediate-use recognizers for
// comparison, bit-twiddles, and raw-argument calls. Keep the runtime assertions
// as coverage of this unsupported case; recursive recognition was withdrawn.
// RUN: split-file %s %t.dir
// RUN: %clang_raw -O2 -c %t.dir/raw.c -o %t.dir/raw.o
// RUN: %clang_ptsan -O2 -fno-slp-vectorize -mllvm -ptsan-strip-pointer-bit-twiddles=12 -I%S %t.dir/consumers.c %t.dir/main.c %t.dir/raw.o -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O2 -fno-slp-vectorize -mllvm -ptsan-strip-pointer-bit-twiddles=12 -mllvm -ptsan-runtime-validate-arith -I%S %t.dir/consumers.c %t.dir/main.c %t.dir/raw.o -o %t.validate
// RUN: %run %t.validate

//--- consumers.c
#include <stdint.h>

volatile uintptr_t saved;

int compare(char *p, char *q, uintptr_t n) {
  uintptr_t address = (uintptr_t)p + n;
  saved = address;
  return address == (uintptr_t)q;
}

int compare_narrow(char *p, char *q, uintptr_t n) {
  return (uint32_t)((uintptr_t)p + n) == (uint32_t)(uintptr_t)q;
}

int compare_shared_narrow(char *p, char *q, char *r, uintptr_t n) {
  uint32_t address = (uint32_t)((uintptr_t)p + n);
  return (address == (uint32_t)(uintptr_t)q) |
         ((address == (uint32_t)(uintptr_t)r) << 1);
}

int compare_float(char *p, char *q, uintptr_t n) {
  return (double)((uintptr_t)p + n) == (double)(uintptr_t)q;
}

uintptr_t mask(char *p, uintptr_t n) {
  uintptr_t address = (uintptr_t)p + n;
  saved = address;
  return address & ~(uintptr_t)UINT32_MAX;
}

uintptr_t shift(char *p, uintptr_t n) {
  return ((uintptr_t)p + n) >> 12;
}

// A name on the existing strip-at-call list. The raw helper reports the bits
// it receives; this does not attempt to exercise the OpenMP runtime.
uintptr_t __kmp_invoke_microtask(uintptr_t);

uintptr_t call_raw(char *p, uintptr_t n) {
  uintptr_t address = (uintptr_t)p + n;
  saved = address;
  return __kmp_invoke_microtask(address);
}

//--- raw.c
#include <stdint.h>
uintptr_t __kmp_invoke_microtask(uintptr_t address) { return address; }

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

extern volatile uintptr_t saved;
int compare(char *, char *, uintptr_t);
int compare_narrow(char *, char *, uintptr_t);
int compare_shared_narrow(char *, char *, char *, uintptr_t);
int compare_float(char *, char *, uintptr_t);
uintptr_t mask(char *, uintptr_t);
uintptr_t shift(char *, uintptr_t);
uintptr_t call_raw(char *, uintptr_t);

int main(void) {
  char *p = malloc(512);
  assert(p && get_id(p));
  uintptr_t raw = ptsan_get_address(p);
  char *q = (char *)(raw + 128);
  assert(compare(p, q, 128));
  assert(saved == (uintptr_t)p + 128);
  assert(compare_narrow(p, q, 128));
  assert(compare_shared_narrow(p, q, q, 128) == 3);
  assert(compare_shared_narrow(p, q, p + 128, 128) == 3);
  assert(compare_float(p, q, 128));
  assert(mask(p, 128) == ((raw + 128) & ~(uintptr_t)UINT32_MAX));
  assert(saved == (uintptr_t)p + 128);
  assert(shift(p, 128) == (raw + 128) >> 12);
  assert(call_raw(p, 128) == raw + 128);
  assert(saved == (uintptr_t)p + 128);
  free(p);
  return 0;
}
