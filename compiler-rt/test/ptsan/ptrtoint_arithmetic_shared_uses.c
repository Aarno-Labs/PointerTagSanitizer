//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// Reassociated subtraction with mixed raw/packed operands is not recognized.
// RUN: %clang_ptsan -O2 -o %t %s
// RUN: %run %t

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "Inputs/ptsan_test_interface.h"

static volatile uintptr_t saved;

__attribute__((noinline)) static uintptr_t distance(char *end, char *p,
                                                  uintptr_t n) {
  uintptr_t adjusted = (uintptr_t)p + n;
  saved = adjusted;
  return (uintptr_t)end - adjusted;
}

int main(void) {
  char *p = malloc(512);
  assert(p);
  assert(get_id(p) != 0);
  char *raw = (char *)(uintptr_t)ptsan_get_address(p);

  // The subtraction observes addresses; the store retains the packed bits.
  assert(distance(raw + 256, p, 128) == 128);
  assert(saved == (uintptr_t)p + 128);
  char *restored = (char *)saved;
  assert(get_id(restored) == get_id(p));
  *restored = 42;
  assert(p[128] == 42);

  free(p);
  return 0;
}
