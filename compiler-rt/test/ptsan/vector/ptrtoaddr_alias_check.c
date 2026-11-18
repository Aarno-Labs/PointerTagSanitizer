// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -O2 -march=x86-64 -S -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_ptsan -O2 -march=x86-64 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-clear-ids-on-arith=0 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -march=x86-64 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-clear-ids-on-arith=0 %s -o %t.id32
// RUN: %run %t.id32
//
// LLVM generates address-only casts for vectorization's overlap check even
// though the source contains no pointer-to-integer casts. Exercise disjoint
// allocations and an overlapping range that must keep scalar iteration order.
#include <assert.h>
#include <stdlib.h>

// CHECK-LABEL: define {{.*}} @add(
// CHECK: ptrtoaddr ptr %dst to i64
// CHECK: ptrtoaddr ptr %src to i64
__attribute__((noinline))
void add(float *dst, const float *src, unsigned long n) {
  for (unsigned long i = 0; i < n; ++i) {
    dst[i] = src[i] + 1.0f;
  }
}

int main(void) {
  float *dst = malloc(64 * sizeof(float));
  float *src = malloc(64 * sizeof(float));
  assert(dst && src);
  for (unsigned i = 0; i < 64; ++i) {
    src[i] = (float)i;
  }
  add(dst, src, 64);
  for (unsigned i = 0; i < 64; ++i) {
    assert(dst[i] == (float)i + 1.0f);
  }
  dst[0] = 7.0f;
  add(dst + 1, dst, 63);
  for (unsigned i = 0; i < 64; ++i) {
    assert(dst[i] == 7.0f + (float)i);
  }
  free(src);
  free(dst);
}
