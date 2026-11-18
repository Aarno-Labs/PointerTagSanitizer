//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Vectorized pointer initialization must preserve each lane's allocation ID
// through storage and qsort callbacks, not just compute the right addresses.

// REQUIRES: x86_64-target-arch
// AVX2 is intentional: without it this loop remains scalar and does not
// exercise storing an entire vector of pointers.
// RUN: %clang_raw -O2 -mavx2 -mllvm -force-vector-width=8 \
// RUN:   -mllvm -force-vector-interleave=1 -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=RAW
// RUN: %clang_ptsan_id32 -O2 -mavx2 -mllvm -force-vector-width=8 \
// RUN:   -mllvm -force-vector-interleave=1 -I%crt_src/include -o %t %s
// RUN: %run %t

// RAW-LABEL: define {{.*}} @initialize(
// RAW: getelementptr inbounds{{( nuw)?}} i8, ptr %base, <4 x i64> <i64 32, i64 28, i64 24, i64 20>
// RAW: store <4 x ptr>

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

enum { Count = 9 };

__attribute__((noinline)) static void initialize(int **out, int *base) {
#pragma clang loop vectorize(enable) vectorize_width(8) interleave(disable)
  for (int i = 0; i < Count; ++i)
    out[i] = base + (Count - i - 1);
}

static int compare(const void *a, const void *b) {
  int *pa = *(int *const *)a;
  int *pb = *(int *const *)b;
  return *pa - *pb;
}

int main(void) {
  int *base = malloc(Count * sizeof(*base));
  int **pointers = malloc(Count * sizeof(*pointers));
  assert(base && pointers);

  for (int i = 0; i < Count; ++i)
    base[i] = i;

  initialize(pointers, base);
  qsort(pointers, Count, sizeof(*pointers), compare);

  uint32_t id = get_id(base);
  for (int i = 0; i < Count; ++i) {
    assert(pointers[i] == base + i);
    assert(get_id(pointers[i]) == id);
  }

  free(pointers);
  free(base);
  return 0;
}
