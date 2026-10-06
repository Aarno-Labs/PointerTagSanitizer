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
// RUN: ./%exe 0 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: ./%exe 1 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt
// RUN: ./%exe 2 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt
// RUN: ./%exe 3 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt

// Each lane points into a different, simultaneously live malloc allocation.
// The vector load therefore contains four independent PTSan IDs and bounds.
// Mode zero accesses the last valid element of each allocation. Modes one and
// two put lane two one-past its allocation for gather and scatter respectively.
// Mode three uses that same invalid pointer in a masked-off lane, proving that
// vector bounds gathering does not consume inactive IDs.

// DISCOVERY-GATHER-LABEL: define {{.*}} @mixed_gather(
// DISCOVERY-GATHER: [[GATHER_MASK:%.*]] = icmp ne <4 x i32>
// DISCOVERY-GATHER: [[GATHER_POINTERS:%.*]] = {{.*}}call <4 x ptr> @llvm.masked.load.v4p0.p0({{.*}}<4 x i1> [[GATHER_MASK]],
// DISCOVERY-GATHER: [[GATHER_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[GATHER_POINTERS]])
// DISCOVERY-GATHER: [[GATHER_IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> [[GATHER_POINTERS]])
// DISCOVERY-GATHER: [[GATHER_BASES:%.*]] = call <4 x i64> @__ptsan_load_base(<4 x i64> [[GATHER_IDS]], <4 x i1> [[GATHER_MASK]])
// DISCOVERY-GATHER: [[GATHER_SPANS:%.*]] = call <4 x i64> @__ptsan_load_span(<4 x i64> [[GATHER_IDS]], <4 x i1> [[GATHER_MASK]])
// DISCOVERY-GATHER: call void (...) @__ptsan_access(<4 x i64> [[GATHER_IDS]], <4 x i64> [[GATHER_BASES]], <4 x i64> [[GATHER_SPANS]], ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 0, <4 x i1> [[GATHER_MASK]])
// DISCOVERY-GATHER-NEXT: {{.*}}call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> [[GATHER_RAW]], i32 4, <4 x i1> [[GATHER_MASK]],
//
// DISCOVERY-SCATTER-LABEL: define {{.*}} @mixed_scatter(
// DISCOVERY-SCATTER: [[SCATTER_MASK:%.*]] = icmp ne <4 x i32>
// DISCOVERY-SCATTER: [[SCATTER_POINTERS:%.*]] = {{.*}}call <4 x ptr> @llvm.masked.load.v4p0.p0({{.*}}<4 x i1> [[SCATTER_MASK]],
// DISCOVERY-SCATTER: [[SCATTER_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[SCATTER_POINTERS]])
// DISCOVERY-SCATTER: [[SCATTER_IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> [[SCATTER_POINTERS]])
// DISCOVERY-SCATTER: [[SCATTER_BASES:%.*]] = call <4 x i64> @__ptsan_load_base(<4 x i64> [[SCATTER_IDS]], <4 x i1> [[SCATTER_MASK]])
// DISCOVERY-SCATTER: [[SCATTER_SPANS:%.*]] = call <4 x i64> @__ptsan_load_span(<4 x i64> [[SCATTER_IDS]], <4 x i1> [[SCATTER_MASK]])
// DISCOVERY-SCATTER: call void (...) @__ptsan_access(<4 x i64> [[SCATTER_IDS]], <4 x i64> [[SCATTER_BASES]], <4 x i64> [[SCATTER_SPANS]], ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 1, <4 x i1> [[SCATTER_MASK]])
// DISCOVERY-SCATTER-NEXT: {{.*}}call void @llvm.masked.scatter.v4i32.v4p0({{.*}}<4 x ptr> [[SCATTER_RAW]], i32 4, <4 x i1> [[SCATTER_MASK]])
//
// OPT-GATHER-LABEL: define {{.*}} @mixed_gather(
// The pointer-array load, data gather, and output store are distinct accesses.
// OPT-GATHER: call void @ptsan_complain_vector(
// OPT-GATHER: call void @ptsan_complain_vector(
// OPT-GATHER: call void @ptsan_complain_vector(
// OPT-GATHER-NOT: call void @ptsan_complain_vector(
// OPT-GATHER: ret void

// OPT-SCATTER-LABEL: define {{.*}} @mixed_scatter(
// The pointer-array load and data scatter are distinct accesses.
// OPT-SCATTER: call void @ptsan_complain_vector(
// OPT-SCATTER: call void @ptsan_complain_vector(
// OPT-SCATTER-NOT: call void @ptsan_complain_vector(
// OPT-SCATTER: ret void

// BAD: Found bad VECTOR access
// GOOD-NOT: Found bad

#include <stddef.h>
#include <stdlib.h>

#define N 4

__attribute__((noinline)) void mixed_gather(int *const *restrict pointers,
                                            const int *restrict active,
                                            int *restrict output, int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    if (active[i])
      output[i] = *pointers[i];
  }
}

__attribute__((noinline)) void mixed_scatter(int *const *restrict pointers,
                                             const int *restrict active,
                                             int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    if (active[i])
      *pointers[i] = 1;
  }
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  int mode = atoi(argv[1]);
  const size_t sizes[N] = {2, 3, 5, 7};
  int *allocations[N];
  int *pointers[N];
  int active[N];
  int output[N];

  for (int i = 0; i < N; ++i) {
    active[i] = 1;
    allocations[i] = (int *)malloc(sizes[i] * sizeof(int));
    if (!allocations[i])
      return 1;
    for (size_t j = 0; j < sizes[i]; ++j)
      allocations[i][j] = (i + 1) * 100 + (int)j;
    pointers[i] = &allocations[i][sizes[i] - 1];
  }

  if (mode != 0)
    pointers[2] = &allocations[2][sizes[2]];
  if (mode == 3)
    active[2] = 0;

  if (mode == 0) {
    mixed_gather(pointers, active, output, N);
    for (int i = 0; i < N; ++i)
      if (output[i] != (i + 1) * 100 + (int)sizes[i] - 1)
        return 2;

    mixed_scatter(pointers, active, N);
    for (int i = 0; i < N; ++i)
      if (*pointers[i] != 1)
        return 3;

    for (int i = 0; i < N; ++i)
      free(allocations[i]);
    return 0;
  }

  if (mode == 1)
    mixed_gather(pointers, active, output, N);
  else if (mode == 2)
    mixed_scatter(pointers, active, N);
  else if (mode == 3) {
    mixed_gather(pointers, active, output, N);
    mixed_scatter(pointers, active, N);
  } else
    return 1;

  _Exit(0);
}
