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
// RUN: ./%exe 63 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe 64 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// The pointer passed to gather_stack refers to one stack allocation, but the
// optimized access itself is an llvm.masked.gather over a vector of derived
// addresses. This is the vector-address counterpart of stack_array.c.
//
// OPT-LABEL: define {{.*}} @gather_stack(
// OPT: call void @ptsan_complain_vector(
// OPT: call <4 x i32> @llvm.masked.gather.v4i32.v4p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: ret void

// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stdlib.h>

#define N 67
#define BUFFER_SIZE 64

__attribute__((noinline)) static void gather_stack(const int *restrict indices,
                                                   const int *restrict buffer,
                                                   int *restrict output,
                                                   int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) \
    unroll(disable)
  for (int i = 0; i < n; ++i)
    output[i] = buffer[indices[i]];
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  int selected = atoi(argv[1]);
  int buffer[BUFFER_SIZE];
  int indices[N];
  int output[N];

  for (int i = 0; i < BUFFER_SIZE; ++i)
    buffer[i] = i;
  for (int i = 0; i < N; ++i)
    indices[i] = i & (BUFFER_SIZE - 1);
  indices[5] = selected;

  gather_stack(indices, buffer, output, N);

  volatile int sink = output[0];
  (void)sink;
  return 0;
}
