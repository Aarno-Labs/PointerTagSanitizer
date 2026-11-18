//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -march=skylake-avx512 -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %s -o %t/late.ll
// RUN: FileCheck %s < %t/late.ll
// RUN: %clang_ptsan_id32 -O2 -march=skylake-avx512 -S -emit-llvm %s -o %t/runtime.ll
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' %t/runtime.ll | %clang_ptsan_id32 -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %t/test -
// RUN: %t/test
// RUN: not %t/test computed 2> %t/computed.err
// RUN: FileCheck %s --check-prefix=BAD < %t/computed.err
// RUN: not %t/test sequential 2> %t/sequential.err
// RUN: FileCheck %s --check-prefix=BAD < %t/sequential.err
// RUN: %clang_ptsan_id32 -O2 -march=skylake-avx512 -mllvm -ptsan-nochecks -S -emit-llvm %s -o %t/nochecks.ll
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' %t/nochecks.ll | %clang_ptsan_id32 -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %t/test.nochecks -
// RUN: %t/test.nochecks

// Generate vector code from C, then lower it for baseline x86-64 execution.
// Conversion dependencies must keep metadata loads after their predicates.
// An unconditional conversion may also serve subsequent masked arithmetic.
//
// CHECK-LABEL: define {{.*}} @computed_mask(
// CHECK: [[MASK:%.*]] = icmp sgt <4 x i32>
// CHECK: [[POINTERS:%.*]] = {{.*}}call <4 x ptr> @llvm.masked.load
// CHECK: [[IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> [[POINTERS]])
// CHECK: [[BASES:%.*]] = call <4 x i64> @__ptsan_load_base(<4 x i64> [[IDS]], <4 x i1> [[MASK]])
// CHECK: [[RAW:%.*]] = call <4 x ptr> @__ptsan_strip_id32(<4 x ptr> [[POINTERS]], <4 x i64> [[IDS]], <4 x i64> [[BASES]])
// CHECK: call <4 x i32> @llvm.masked.gather{{.*}}(<4 x ptr> align 4 [[RAW]], <4 x i1> [[MASK]],
//
// CHECK-LABEL: define {{.*}} @sequential_masks(
// CHECK: [[SHARED_RAW:%.*]] = call <4 x ptr> @__ptsan_strip_id32(
// CHECK: [[FIRST:%.*]] = {{.*}}call <4 x i32> @llvm.masked.gather{{.*}}(<4 x ptr> {{(align [0-9]+ )?}}[[SHARED_RAW]],
// CHECK: [[LATER_MASK:%.*]] = icmp sgt <4 x i32> {{%.*}}, [[FIRST]]
// CHECK-NOT: call <4 x ptr> @__ptsan_strip_id32(
// CHECK: [[NEXT:%.*]] = getelementptr inbounds{{( nuw)?}} i8, <4 x ptr> [[SHARED_RAW]], i64 4
// CHECK: call <4 x i32> @llvm.masked.gather{{.*}}(<4 x ptr> align 4 [[NEXT]], <4 x i1> [[LATER_MASK]],
// BAD: Found bad VECTOR access

#include <assert.h>
#include <stdlib.h>

__attribute__((noinline))
void computed_mask(int *restrict output,
                   int *const *restrict pointers, const int *restrict flags,
                   int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    int *p = pointers[i];
    if (flags[i] > 0)
      output[i] = *p;
  }
}

__attribute__((noinline))
void sequential_masks(int *restrict output, int *const *restrict pointers,
                      const int *restrict flags, int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    int *p = pointers[i];
    int first = *p;
    if (flags[i] > first)
      output[i] = p[1];
  }
}

int main(int argc, char **argv) {
  enum { N = 17 };
  int *values = malloc(2 * sizeof(int));
  int **pointers = malloc(N * sizeof(int *));
  int *flags = malloc(N * sizeof(int));
  int *output = malloc(N * sizeof(int));
  assert(values && pointers && flags && output);
  values[0] = 17;
  values[1] = 29;
  for (int i = 0; i < N; ++i) {
    flags[i] = i % 3 ? 20 : 0;
    pointers[i] = flags[i] ? values : NULL;
    output[i] = -1;
  }
  if (argc > 1 && argv[1][0] == 'c')
    pointers[1] = values + 2;
  computed_mask(output, pointers, flags, N);
  for (int i = 0; i < N; ++i) {
    assert(output[i] == (flags[i] ? 17 : -1));
    pointers[i] = values;
    output[i] = -1;
  }
  if (argc > 1 && argv[1][0] == 's') {
    pointers[1] = values + 1;
    flags[1] = 100;
  }
  sequential_masks(output, pointers, flags, N);
  for (int i = 0; i < N; ++i)
    assert(output[i] == (flags[i] ? 29 : -1));
  free(output);
  free(flags);
  free(pointers);
  free(values);
  return 0;
}
