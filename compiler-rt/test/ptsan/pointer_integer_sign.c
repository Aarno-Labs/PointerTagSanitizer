//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O3 -mllvm -ptsan-clear-ids-on-cmp=0 -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan -O3 %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

// Perl's newSVuv selects signed or unsigned storage from the sign bit of a
// pointer cast to an integer. LLVM folds this into a pointer comparison against
// -1. Testing a decoded address while storing packed bits chooses the wrong
// numeric type. The separate caller checks the returned integer representation.

//--- helper.c
#include <stdint.h>

uintptr_t classify(void *p, int *is_unsigned) {
  uintptr_t bits = (uintptr_t)p;
  *is_unsigned = bits > INTPTR_MAX;
  return bits;
}

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

uintptr_t classify(void *, int *);

int main(void) {
  void *p = malloc(16);
  assert(p);
  int is_unsigned;
  uintptr_t bits = classify(p, &is_unsigned);
  assert(is_unsigned == (bits > INTPTR_MAX));
  free(p);
}
