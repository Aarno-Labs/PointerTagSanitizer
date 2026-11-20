//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O3 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=8 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o ptsan.ll %s
// RUN: FileCheck %s --check-prefix=OPT < ptsan.ll
// RUN: %clang_ptsan -O3 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=8 -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-abort-on-error -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe 0 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: not ./%exe 1 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// The two masked loads start one element apart. Their checks should merge by
// widening each lane from four to eight bytes while retaining the original
// four-byte distance between consecutive lanes. The masked output store keeps
// its independent check.

// OPT-LABEL: define {{.*}} @sum_adjacent(
// OPT: getelementptr i8, ptr {{.*}}, <8 x i64> <i64 0, i64 4, i64 8, i64 12, i64 16, i64 20, i64 24, i64 28>
// OPT: @llvm.uadd.with.overflow.v8i64({{.*}}<i64 8, i64 8, i64 8, i64 8, i64 8, i64 8, i64 8, i64 8>)
// OPT: call void @ptsan_complain_vector(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: call <8 x i32> @llvm.masked.load.v8i32.p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: call <8 x i32> @llvm.masked.load.v8i32.p0(
// OPT: call void @ptsan_complain_vector(
// OPT: call void @llvm.masked.store.v8i32.p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: ret void

// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stddef.h>
#include <stdlib.h>

#define N 8

__attribute__((noinline)) void sum_adjacent(const int *restrict active,
                                            const int *restrict input,
                                            int *restrict output,
                                            size_t count) {
#pragma clang loop vectorize(enable) vectorize_width(8) interleave_count(1) \
    unroll(disable)
  for (size_t i = 0; i < count; ++i) {
    if (active[i]) {
      output[i] = input[i] + input[i + 1];
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  int offset = atoi(argv[1]);
  int active[N] = {1, 1, 1, 1, 1, 1, 1, 1};
  int input[N + 1] = {0};
  int output[N] = {0};

  sum_adjacent(active, input + offset, output, N);
  volatile int sink = output[0];
  (void)sink;
  _Exit(0);
}
