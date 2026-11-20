//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The seed selects the final 65536-ID bucket before any other fresh bucket.
// UINT32_MAX is a usable ID32 ID; the exclusive bucket end needs 64 bits.
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

enum { Count = 65536 };
static unsigned char *Pointers[Count];

int main(void) {
  unsigned top = Count;
  for (unsigned i = 0; i != Count; ++i) {
    Pointers[i] = malloc(1);
    assert(Pointers[i]);
    assert(get_id(Pointers[i]) != 0);
    *Pointers[i] = 42;
    if (get_id(Pointers[i]) == UINT32_MAX) {
      assert(top == Count);
      top = i;
    }
  }
  assert(top != Count);
  assert(ptsan_get_size(UINT32_MAX) == 1);
  assert(*Pointers[top] == 42);
  for (unsigned i = 0; i != Count; ++i)
    free(Pointers[i]);
  assert(ptsan_get_size(UINT32_MAX) == 0);
  return 0;
}
