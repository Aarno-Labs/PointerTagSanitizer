//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o ptsan.ll %s
// RUN: FileCheck %s --check-prefix=OPT < ptsan.ll
// RUN: %clang_ptsan -O2 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe 0 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe 17 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// The affine stride proves that distinct loop iterations access distinct
// elements, so ordinary vectorization may legally form this gather/scatter
// pair without vectorize(assume_safety). The load and conditional store use
// exactly the same vector of addresses. Once gather/scatter checks are
// supported, the existing same-bounds optimization should merge their equal
// VectorAccessRanges into one check. The conditional output write is a
// separate contiguous masked store and therefore retains its own check.
//
// OPT-LABEL: define {{.*}} @same_access(
// OPT: call void @ptsan_complain_vector(
// OPT: {{.*}}call <4 x i32> @llvm.masked.gather.v4i32.v4p0(
// OPT-NEXT: {{.*}}call void @llvm.masked.scatter.v4i32.v4p0(
// OPT: call void @ptsan_complain_vector(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: ret void

// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stdlib.h>

#define N 4
#define STRIDE 17
#define BUFFER_SIZE (N * STRIDE)

__attribute__((noinline)) void same_access(int offset, const int *restrict active,
                                           int *restrict buffer,
                                           int *restrict output, int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i) {
    int *address = &buffer[offset + i * STRIDE];
    if (active[i]) {
      int value = *address;
      *address = 0;
      output[i] = value;
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  int offset = atoi(argv[1]);
  int active[N];
  int buffer[BUFFER_SIZE];
  int output[N];

  for (int i = 0; i < N; ++i)
    active[i] = 1;
  for (int i = 0; i < BUFFER_SIZE; ++i)
    buffer[i] = 100 + i;

  same_access(offset, active, buffer, output, N);

  volatile int sink = output[0];
  (void)sink;
  return 0;
}
