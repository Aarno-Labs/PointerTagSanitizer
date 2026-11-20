//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// After ID reuse, the new allocation must work and the stale packed pointer
// must fail its bounds check. Wait for actual reuse rather than depending on
// allocator order, then verify the seeded payload intervals are disjoint.

// RUN: split-file %s %t.dir
// RUN: %clang_ptsan_id32 -O2 -I%S -mllvm -ptsan-random-seed=1 \
// RUN:   -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 \
// RUN:   -mllvm -ptsan-stack-id-thread-buckets=0 \
// RUN:   %t.dir/main.c %t.dir/read.c -o %t
// RUN: %run %t
// RUN: not %run %t stale 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

//--- main.c
#include "Inputs/id_reuse.h"
#include <assert.h>
#include <stdlib.h>

int read_byte(const char *p);

int main(int argc, char **argv) {
  char *p = malloc(16);
  assert(p);
  uint32_t id = get_id(p);
  assert(id != 0 && id != ptsan_default_id());
  uint32_t old_payload = ptsan_get_payload(p);
  // Preserve packed bits, not an address decoded before the free.
  volatile uintptr_t stale = (uintptr_t)p;
  free(p);

  char *replacement = allocate_reusing_id(id);
  assert(replacement && get_id(replacement) == id);
  uint64_t origin = ptsan_get_origin(id);
  uint64_t size = ptsan_get_size(id);
  // Different origins alone are insufficient: the valid intervals could
  // overlap. Verify rejection is required for this seeded allocation sequence.
  assert(old_payload < origin || old_payload >= origin + size);
  replacement[0] = 42;
  assert(read_byte(replacement) == 42);
  if (argc > 1) {
    (void)read_byte((const char *)stale);
    assert(!"stale pointer access should have complained");
  }
  free(replacement);
  return 0;
}

//--- read.c
// Decode only after ID reuse; the compiler cannot reuse a pre-free raw address.
int read_byte(const char *p) {
  return *(const volatile char *)p;
}
