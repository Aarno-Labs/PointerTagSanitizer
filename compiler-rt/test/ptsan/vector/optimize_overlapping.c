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
// RUN: ./%exe 127 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe 128 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// This is a naturally vectorized counterpart of optimize_overlapping.c. Each
// lane reads the same address through a four-byte range and then an eight-byte
// range. The existing overlapping-range optimization should operate unchanged
// once VectorAccessRange::cmp is available.
//
// The loop is read-only: it needs no assume_safety promise about indexed
// load/store independence.

// The wider access subsumes the immediately preceding narrow access.
// OPT-LABEL: define {{.*}} @overlapping_reads(
// OPT: call void @ptsan_complain_vector(
// OPT: call <4 x i32> @llvm.masked.gather.v4i32.v4p0(
// OPT: call <4 x i64> @llvm.masked.gather.v4i64.v4p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: ret void

// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stdint.h>
#include <stdlib.h>

#define N 67
#define BUFFER_SIZE 128

typedef union {
  int64_t wide;
  int32_t narrow[2];
} Cell;

__attribute__((noinline)) static void
overlapping_reads(const int *restrict indices, const Cell *restrict buffer,
                  int *restrict narrow_output, int64_t *restrict wide_output,
                  int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) \
    unroll(disable)
  for (int i = 0; i < n; ++i) {
    const Cell *address = &buffer[indices[i]];
    narrow_output[i] = address->narrow[0];
    wide_output[i] = address->wide;
  }
}

static void initialize(Cell *buffer) {
  for (int i = 0; i < BUFFER_SIZE; ++i)
    buffer[i].wide = 0x100000000LL + i;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  int selected = atoi(argv[1]);
  Cell buffer[BUFFER_SIZE];
  int indices[N];
  int narrow_output[N];
  int64_t wide_output[N];

  for (int i = 0; i < N; ++i)
    indices[i] = i;
  indices[5] = selected;

  initialize(buffer);
  overlapping_reads(indices, buffer, narrow_output, wide_output, N);

  volatile int64_t sink = narrow_output[0] + wide_output[0];
  (void)sink;
  return 0;
}
