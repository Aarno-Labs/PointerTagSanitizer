//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Free clears bounds and returns IDs to the reuse queue regardless of object
// size or alignment. Exercise small, large, and strongly aligned allocations
// until reuse occurs, including a larger configured origin quantum. Cache
// batching may delay reuse even when the configured reuse distance is zero.

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 -I%crt_src/include -o %t %s
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 -I%crt_src/include -o %t.quantum %s
// RUN: %run %t.quantum

#include "Inputs/id_reuse.h"
#include <assert.h>
#include <stdlib.h>

static void check_reuse(void *p) {
  assert(p != NULL);
  PTSanID id = get_id(p);
  free(p);
  assert(ptsan_get_size(id) == 0);

  void *replacement = allocate_reusing_id(id);
  assert(replacement != NULL);
  assert(get_id(replacement) == id);
  free(replacement);
}

int main(void) {
  check_reuse(malloc(16));
  check_reuse(malloc(4 * 1024 * 1024));

  void *aligned = NULL;
  int result = posix_memalign(&aligned, 16 * 1024 * 1024, 16);
  assert(result == 0);
  check_reuse(aligned);
  return 0;
}
