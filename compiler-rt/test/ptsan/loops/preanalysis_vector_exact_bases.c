//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -march=skylake-avx512 \
// RUN:   -fno-discard-value-names -mllvm -force-vector-width=4 \
// RUN:   -mllvm -force-vector-interleave=1 -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o optimized.ll %s
// RUN: FileCheck %s --check-prefix=OPT < optimized.ll
// RUN: %clang_raw -fsanitize=ptsan -g -O3 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 -o test %s
// RUN: ./test 2> good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: not ./test bad 2> bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// The loop vectorizer widens each input stream independently. Every resulting
// vector load still has one of the original scalar base pointers, so ordinary
// exact-base preanalysis covers these vector operations without the collective
// complete-summary fallback.

// OPT-LABEL: define {{.*}} @transpose(
// OPT: call void @llvm.sideeffect() [ "ptsan.loop.preheader"
// OPT-COUNT-5: call void (...) @__ptsan_loop_preanalysis(
// OPT-COUNT-4: load <4 x i32>, ptr
// OPT-NOT: call void (...) @__ptsan_loop_preanalysis(
// OPT-NOT: call void (...) @__ptsan_access(
// OPT: ret void

// GOOD-NOT: Found bad
// BAD: Found bad {{.*}} access

#include <string.h>

#define ARRAY_SIZE 64

volatile unsigned preanalysis_vector_count = 8;
volatile int preanalysis_vector_sink;

__attribute__((noinline))
static void transpose(int *output, const int *x, const int *y, const int *z,
                      const int *w, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    output[4 * i] = x[i];
    output[4 * i + 1] = y[i];
    output[4 * i + 2] = z[i];
    output[4 * i + 3] = w[i];
  }
}

int main(int argc, char **argv) {
  int output[4 * ARRAY_SIZE];
  int x[ARRAY_SIZE];
  int y[ARRAY_SIZE];
  int z[ARRAY_SIZE];
  int w[ARRAY_SIZE];
  unsigned count = preanalysis_vector_count;

  memset(output, 0, sizeof(output));
  memset(x, 0, sizeof(x));
  memset(y, 0, sizeof(y));
  memset(z, 0, sizeof(z));
  memset(w, 0, sizeof(w));
  if (argc > 1 && argv[1][0] == 'b') {
    count = ARRAY_SIZE + 1;
  }

  transpose(output, x, y, z, w, count);
  preanalysis_vector_sink = output[0];
  return 0;
}
