//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 -march=skylake-avx512 -fno-discard-value-names \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-stop-after=discovery -S -emit-llvm -o discovery.ll %s
// RUN: FileCheck %s --check-prefix=DISCOVERY-GATHER \
// RUN:   --check-prefix=DISCOVERY-SCATTER < discovery.ll
// RUN: %clang_ptsan -O2 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o ptsan.ll %s
// RUN: FileCheck %s --check-prefix=OPT-GATHER --check-prefix=OPT-SCATTER < ptsan.ll
// RUN: %clang_ptsan -O2 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe g -1 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe s -1 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe g 63 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe s 63 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe g 64 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt
// RUN: ./%exe s 64 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// A negative index creates an invalid/poison-capable address in lane one, but
// the source guard makes that lane inactive. Bounds gathering and checking
// must apply the same mask before consuming the address or its ID. A positive
// one-past index is active and must be reported for both gather and scatter.

// DISCOVERY-GATHER-LABEL: define {{.*}} @guarded_gather(
// DISCOVERY-GATHER: [[GATHER_MASK:%.*]] = icmp sgt <4 x i32>
// DISCOVERY-GATHER: [[GATHER_POINTERS:%.*]] = getelementptr inbounds i32, ptr {{%.*}}, <4 x i64>
// DISCOVERY-GATHER: [[GATHER_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[GATHER_POINTERS]])
// DISCOVERY-GATHER: call void (...) @__ptsan_access(i64 {{%.*}}, i64 {{%.*}}, i64 {{%.*}}, ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 0, <4 x i1> [[GATHER_MASK]])
// DISCOVERY-GATHER-NEXT: {{.*}}call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> [[GATHER_RAW]], i32 4, <4 x i1> [[GATHER_MASK]],
//
// DISCOVERY-SCATTER-LABEL: define {{.*}} @guarded_scatter(
// DISCOVERY-SCATTER: [[SCATTER_MASK:%.*]] = icmp sgt <4 x i32>
// DISCOVERY-SCATTER: [[SCATTER_POINTERS:%.*]] = getelementptr inbounds i32, ptr {{%.*}}, <4 x i64>
// DISCOVERY-SCATTER: [[SCATTER_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[SCATTER_POINTERS]])
// DISCOVERY-SCATTER: call void (...) @__ptsan_access(i64 {{%.*}}, i64 {{%.*}}, i64 {{%.*}}, ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 1, <4 x i1> [[SCATTER_MASK]])
// DISCOVERY-SCATTER-NEXT: {{.*}}call void @llvm.masked.scatter.v4i32.v4p0({{.*}}<4 x ptr> [[SCATTER_RAW]], i32 4, <4 x i1> [[SCATTER_MASK]])
//
// OPT-GATHER-LABEL: define {{.*}} @guarded_gather(
// OPT-GATHER: call void @ptsan_complain_vector(
// The conditional output write is a distinct contiguous masked store.
// OPT-GATHER: call void @ptsan_complain_vector(
// OPT-GATHER-NOT: call void @ptsan_complain_vector(
// OPT-GATHER: ret void

// OPT-SCATTER-LABEL: define {{.*}} @guarded_scatter(
// OPT-SCATTER: call void @ptsan_complain_vector(
// OPT-SCATTER-NOT: call void @ptsan_complain_vector(
// OPT-SCATTER: ret void

// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stdlib.h>

#define N 4
#define BUFFER_SIZE 64
#define SENTINEL (-777)

__attribute__((noinline)) void guarded_gather(const int *restrict indices,
                                              const int *restrict buffer,
                                              int *restrict output, int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    int index = indices[i];
    if (index >= 0)
      output[i] = buffer[index];
  }
}

__attribute__((noinline)) void guarded_scatter(const int *restrict indices,
                                               int *restrict buffer, int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    int index = indices[i];
    if (index >= 0)
      buffer[index] = 1;
  }
}

int main(int argc, char **argv) {
  if (argc != 3)
    return 1;

  char operation = argv[1][0];
  int selected = atoi(argv[2]);
  int indices[N] = {0, selected, 2, 3};
  int buffer[BUFFER_SIZE];
  int output[N];

  for (int i = 0; i < BUFFER_SIZE; ++i)
    buffer[i] = 100 + i;
  for (int i = 0; i < N; ++i)
    output[i] = SENTINEL;

  if (operation == 'g') {
    guarded_gather(indices, buffer, output, N);
    if (selected < 0 && output[1] != SENTINEL)
      return 2;
    if (selected >= 0 && selected < BUFFER_SIZE && output[1] != 100 + selected)
      return 3;
  } else if (operation == 's') {
    guarded_scatter(indices, buffer, N);
    if (selected >= 0 && selected < BUFFER_SIZE && buffer[selected] != 1)
      return 4;
  } else {
    return 1;
  }

  return 0;
}
