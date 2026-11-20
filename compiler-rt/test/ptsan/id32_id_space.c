//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Keep more than 65536 heap allocations live to exercise the larger ID space.
// Fresh IDs must remain distinct, and the default reuse delay must prevent
// immediate recycling of the first two freed IDs.

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-instrument-globals=0 -I%crt_src/include -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

enum { NumPointers = 70000 };

static int compare_ids(const void *a, const void *b) {
  PTSanID x = *(const PTSanID *)a, y = *(const PTSanID *)b;
  return (x > y) - (x < y);
}

int main(void) {
  void **ptrs = malloc(sizeof(*ptrs) * NumPointers);
  assert(ptrs != NULL);
  PTSanID *ids = malloc(sizeof(*ids) * NumPointers);
  assert(ids);

  for (unsigned i = 0; i < NumPointers; ++i) {
    ptrs[i] = malloc(1);
    assert(ptrs[i] != NULL);
    ids[i] = get_id(ptrs[i]);
  }
  // Live IDs must be distinct; caches need not hand them out in numeric order.
  qsort(ids, NumPointers, sizeof(*ids), compare_ids);
  assert(ids[0] != 0);
  for (unsigned i = 1; i != NumPointers; ++i) {
    assert(ids[i - 1] != ids[i]);
  }

  uint32_t freed0 = get_id(ptrs[0]);
  uint32_t freed1 = get_id(ptrs[1]);
  free(ptrs[0]);
  free(ptrs[1]);
  ptrs[0] = malloc(1);
  ptrs[1] = malloc(1);
  assert(get_id(ptrs[0]) != freed0 && get_id(ptrs[0]) != freed1);
  assert(get_id(ptrs[1]) != freed0 && get_id(ptrs[1]) != freed1);

  for (unsigned i = 0; i < NumPointers; ++i)
    free(ptrs[i]);
  free(ptrs);
  free(ids);
  return 0;
}
