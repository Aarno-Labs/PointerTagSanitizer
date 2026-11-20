//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Compare tagged and raw representations of the same address after floating
// conversion or integer truncation. The comparison policy must strip before
// those casts lose pointer bits, rather than strip the converted numbers.
// RUN: %clang_ptsan -O2 -fno-slp-vectorize -mllvm -ptsan-clear-ids-on-cmp -o %t %s
// RUN: %run %t
// RUN: %clang_ptsan -O2 -fno-slp-vectorize -mllvm -ptsan-clear-ids-on-cmp \
// RUN:   -mllvm -ptsan-runtime-validate-arith -o %t.validate %s
// RUN: %run %t.validate

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "Inputs/ptsan_test_interface.h"

// Keep scalar cast chains: SLP would introduce extractelement between the
// casts and comparison, which is outside this cast-only compatibility policy.
__attribute__((noinline)) static int compare_float(void *a, void *b) {
  return (double)(uintptr_t)a == (double)(uintptr_t)b;
}

__attribute__((noinline)) static int compare_truncated(void *a, void *b) {
  return (uint32_t)(uintptr_t)a == (uint32_t)(uintptr_t)b;
}

int main(void) {
  char *p = malloc(32);
  assert(p);
  assert(get_id(p) != 0);
  void *raw = (void *)(uintptr_t)ptsan_get_address(p);

  assert(compare_float(p, raw));
  assert(compare_truncated(p, raw));
  free(p);
  return 0;
}
