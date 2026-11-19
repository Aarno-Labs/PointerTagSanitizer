//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// XFAIL: *
// An address hidden behind integer arithmetic is not stripped for inline asm.
// The stored integer must retain its tag, but a real asm instruction needs the
// raw address. This records the missing boundary conversion, distinct from the
// empty-asm case where preserving tags is intentional.
// RUN: %clang_ptsan -O2 -o %t %s
// RUN: %run %t

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

static volatile uintptr_t saved;

__attribute__((noinline)) static uintptr_t observe(char *p, uintptr_t n) {
  uintptr_t address = (uintptr_t)p + n;
  saved = address;
  // A real instruction keeps this outside the empty-asm preservation rule.
  // TEST leaves the register unchanged, exposing the bits handed to asm.
  __asm__ volatile("testq %0, %0" : "+r"(address) : : "cc");
  return address;
}

int main(void) {
  char *p = malloc(512);
  assert(p && get_id(p));
  assert(observe(p, 128) == ptsan_get_address(p) + 128);
  assert(saved == (uintptr_t)p + 128);
  free(p);
  return 0;
}
