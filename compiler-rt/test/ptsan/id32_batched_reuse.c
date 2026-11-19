//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Refill and drain the ID32 caches far enough to grow the shared recycled-ID
// queue. Live batches must be disjoint, and retired IDs must become reusable.
// Cache order is deliberately not part of this test's contract.
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-id-reuse-distance=2 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 %s -o %t
// RUN: %run %t

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

extern void ptsan_get_stack_ids(uint32_t *, uint64_t);
extern void ptsan_free_stack_ids(uintptr_t);

enum { Count = 16384 };
static uint32_t first[Count], second[Count];

__attribute__((disable_sanitizer_instrumentation))
static int compare(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

__attribute__((disable_sanitizer_instrumentation))
int main(void) {
  uintptr_t start = ptsan_stack_id_log.cursor;
  ptsan_get_stack_ids(first, Count);
  ptsan_free_stack_ids(start);
  ptsan_get_stack_ids(second, Count);
  qsort(first, Count, sizeof(*first), compare);
  qsort(second, Count, sizeof(*second), compare);
  unsigned reused = 0;
  for (unsigned i = 0, j = 0; i != Count; ++i) {
    assert(second[i] != 0);
    assert(i == 0 || second[i] != second[i - 1]);
    while (j != Count && first[j] < second[i]) {
      ++j;
    }
    reused += j != Count && first[j] == second[i];
  }
  assert(reused != 0);
  ptsan_free_stack_ids(start);
  assert(ptsan_stack_id_log.cursor == start);
  return 0;
}
