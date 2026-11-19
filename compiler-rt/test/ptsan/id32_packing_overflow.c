//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 -fno-strict-overflow %s -o %t
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

// Non-inbounds arithmetic happens on raw addresses; returning the result
// requires packing it again, including the representability check.
__attribute__((noinline)) static char *offset(char *p, int64_t amount) {
  return p + amount;
}

__attribute__((noinline)) static char read_byte(char *p) {
  return *(volatile char *)p;
}

int main(int argc, char **argv) {
  char *p = malloc(32);
  assert(p);
  uint32_t id = get_id(p);
  int64_t origin = ptsan_get_origin(id);

  // Both unrepresentable payloads truncate to all ones. Non-strict mode keeps
  // their raw addresses; strict mode returns null instead of wrapping them.
  char *underflow = offset(p, -origin - 1);
  char *overflow = offset(p, INT64_C(0x1ffffffff) - origin);
#ifdef TEST_STRICT
  assert(underflow == 0);
  assert(overflow == 0);
#else
  assert((uintptr_t)underflow == ptsan_get_address(p) - origin - 1);
  assert((uintptr_t)overflow ==
         ptsan_get_address(p) + INT64_C(0x1ffffffff) - origin);
#endif
  assert(get_id(underflow) == 0);
  assert(get_id(overflow) == 0);

  char *inside = offset(p, 7);
  assert(get_id(inside) == id);
  assert(ptsan_get_address(inside) == ptsan_get_address(p) + 7);
#ifndef TEST_STRICT
  char *raw = (char *)(uintptr_t)ptsan_get_address(p);
  assert(offset(raw, 7) == raw + 7);
  assert(get_id(offset(raw, 7)) == 0);

  char *sentinel = (char *)UINTPTR_MAX;
  assert(offset(sentinel, 0) == sentinel);
  assert(get_id(sentinel) == 0);
  assert(ptsan_get_address(sentinel) == UINTPTR_MAX);
#endif

#ifdef TEST_STRICT
  if (argc > 1) {
    (void)read_byte(overflow);
    assert(0 && "null access must complain");
  }
#endif
  free(p);
  return 0;
}
