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

// This is a reduced version of the obstacle-copy path in 519.lbm_r. The loop
// vectorizer gives each field access a distinct vector of GEPs, even though
// all four loads have the same allocation base and execution mask. PTSan can
// merge the four source fields into one per-lane range check. The independent
// output check remains, leaving two checks total.

// OPT-LABEL: define {{.*}} @copy_active_cells(
// OPT: @llvm.uadd.with.overflow.v8i64({{.*}}<i64 56, i64 56, i64 56, i64 56, i64 56, i64 56, i64 56, i64 56>)
// OPT: call void @ptsan_complain_vector(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: call <8 x double> @llvm.masked.gather.v8f64.v8p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: call <8 x double> @llvm.masked.gather.v8f64.v8p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: call <8 x double> @llvm.masked.gather.v8f64.v8p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: call <8 x double> @llvm.masked.gather.v8f64.v8p0(
// OPT: call void @ptsan_complain_vector(
// OPT: call void @llvm.masked.store.v8f64.p0(
// OPT-NOT: call void @ptsan_complain_vector(
// OPT: ret void
//
// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stddef.h>
#include <stdlib.h>

typedef struct {
  double field[8];
} Cell;

__attribute__((noinline)) void copy_active_cells(const int *restrict active,
                                                 const Cell *restrict src,
                                                 double *restrict output,
                                                 size_t count) {
#pragma clang loop vectorize(enable) vectorize_width(8) interleave_count(1) \
    unroll(disable)
  for (size_t i = 0; i < count; ++i) {
    if (active[i]) {
      output[i] = src[i].field[0] + src[i].field[2] + src[i].field[4] +
                  src[i].field[6];
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  int offset = atoi(argv[1]);
  int active[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  Cell src[8] = {0};
  double output[8] = {0};

  copy_active_cells(active, src + offset, output, 8);
  volatile double sink = output[0];
  (void)sink;
  _Exit(0);
}
