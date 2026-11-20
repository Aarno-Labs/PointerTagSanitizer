//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Reserve a large stack-ID range to force a heap ID above bit 24. Allocation,
// ID extraction, and free must preserve those bits instead of truncating them.

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-id-bucket-size=16777216 -I%crt_src/include -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

int main(void) {
  void *p = malloc(16);
  assert(p != NULL);
  assert(get_id(p) > (1U << 24));
  free(p);
  return 0;
}
