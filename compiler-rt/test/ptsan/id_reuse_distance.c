//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-id-reuse-distance=4 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 -I%crt_src/include -o %t %s
// RUN: %run %t

// Reuse distance controls the target recycled-ID candidate pool, not a minimum
// allocation delay. Check that a retired ID remains eventually reusable.
#include "Inputs/id_reuse.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
  void *first = malloc(1);
  assert(first != NULL);
  uint32_t first_id = get_id(first);
  free(first);

  void *reused = allocate_reusing_id(first_id);
  assert(reused != NULL);
  assert(get_id(reused) == first_id);
  free(reused);
  return 0;
}
