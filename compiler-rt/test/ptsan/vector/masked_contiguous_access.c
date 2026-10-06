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
// RUN: FileCheck %s --check-prefix=DISCOVERY-LOAD \
// RUN:   --check-prefix=DISCOVERY-STORE < discovery.ll
// RUN: %clang_ptsan -O2 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe l 0 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: ./%exe s 0 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: ./%exe l 1 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt
// RUN: ./%exe s 1 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// This is the contiguous-address counterpart of masked_access.c. LLVM is
// allowed to form addresses outside the allocation for inactive lanes of a
// masked load or store. PTSan must apply the intrinsic's mask to its checks,
// while still reporting the same address when its lane is active.

// DISCOVERY-LOAD-LABEL: define {{.*}} @guarded_load(
// DISCOVERY-LOAD: [[LOAD_MASK:%.*]] = and <4 x i1>
// DISCOVERY-LOAD: [[LOAD_RAW:%.*]] = call ptr @__ptsan_strip(
// DISCOVERY-LOAD: call void (...) @__ptsan_access({{.*}}i64 4, i32 1, i32 0, <4 x i1> [[LOAD_MASK]])
// DISCOVERY-LOAD-NEXT: {{.*}}call <4 x i32> @llvm.masked.load.v4i32.p0(ptr [[LOAD_RAW]], i32 4, <4 x i1> [[LOAD_MASK]],
// DISCOVERY-LOAD: [[STORE_RAW:%.*]] = call ptr @__ptsan_strip(
// DISCOVERY-LOAD: call void (...) @__ptsan_access({{.*}}i64 4, i32 1, i32 1, <4 x i1> [[LOAD_MASK]])
// DISCOVERY-LOAD-NEXT: {{.*}}call void @llvm.masked.store.v4i32.p0({{.*}}ptr [[STORE_RAW]], i32 4, <4 x i1> [[LOAD_MASK]])

// DISCOVERY-STORE-LABEL: define {{.*}} @guarded_store(
// DISCOVERY-STORE: [[STORE_MASK:%.*]] = and <4 x i1>
// DISCOVERY-STORE: [[ONLY_STORE_RAW:%.*]] = call ptr @__ptsan_strip(
// DISCOVERY-STORE: call void (...) @__ptsan_access({{.*}}i64 4, i32 1, i32 1, <4 x i1> [[STORE_MASK]])
// DISCOVERY-STORE-NEXT: {{.*}}call void @llvm.masked.store.v4i32.p0({{.*}}ptr [[ONLY_STORE_RAW]], i32 4, <4 x i1> [[STORE_MASK]])

// BAD: Found bad VECTOR access
// GOOD-NOT: Found bad

#include <stdlib.h>

#define N 64

static __attribute__((noinline)) void
guarded_load(const int *restrict input, int *restrict output, int delta) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) \
    unroll(disable)
  for (int i = 0; i < N; ++i) {
    if ((i >> 3) > 1 && (i & 7) > 0) {
      output[i] = input[i + delta];
    }
  }
}

static __attribute__((noinline)) void guarded_store(int *output, int delta) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) \
    unroll(disable)
  for (int i = 0; i < N; ++i) {
    if ((i >> 3) > 1 && (i & 7) > 0) {
      output[i + delta] = i;
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    return 1;
  }

  int *input = (int *)malloc(N * sizeof(int));
  int *output = (int *)malloc(N * sizeof(int));
  if (!input || !output) {
    return 1;
  }

  for (int i = 0; i < N; ++i) {
    input[i] = 100 + i;
    output[i] = 0;
  }

  int delta = atoi(argv[2]) ? 48 : -17;
  if (argv[1][0] == 'l') {
    guarded_load(input, output, delta);
    if (delta == -17 && output[17] != input[0]) {
      return 2;
    }
  } else if (argv[1][0] == 's') {
    guarded_store(output, delta);
    if (delta == -17 && output[0] != 17) {
      return 3;
    }
  } else {
    return 1;
  }

  _Exit(0);
}
