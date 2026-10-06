//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 -march=skylake-avx512 -fno-unroll-loops \
// RUN:   -fno-vectorize -fno-slp-vectorize \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o ir.ll %s
// RUN: FileCheck %s --check-prefix=IR < ir.ll
// RUN: %clang_ptsan -O2 -march=skylake-avx512 -fno-unroll-loops \
// RUN:   -fno-vectorize -fno-slp-vectorize \
// RUN:   -mllvm -ptsan-abort-on-error -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe c 0 1 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: ./%exe r 0 1 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: not ./%exe c 1 1 2>bad.txt
// RUN: FileCheck %s --check-prefix=PREANALYZED < bad.txt
// RUN: not ./%exe r 1 1 2>bad.txt
// RUN: FileCheck %s --check-prefix=VECTOR < bad.txt

// Explicit SIMD intrinsics exist before loop preanalysis, unlike masked
// accesses formed later by LoopVectorize. A constant partial mask can be
// summarized by one scalar envelope. A runtime mask cannot, so preanalysis
// must fail closed and leave the vector check in the loop. The constant mask
// selects lanes two and three, making the bad case test both the envelope's
// nonzero offset and its size. Masked stores are covered separately by
// masked_contiguous_access.c; the preanalysis decision is independent of the
// access kind.

// IR-LABEL: define {{.*}} @constant_mask_load(
// IR: call void (...) @__ptsan_loop_preanalysis(
// IR-NOT: call void (...) @__ptsan_access(
// IR: call <16 x i32> @llvm.masked.load.v16i32.p0(

// IR-LABEL: define {{.*}} @runtime_mask_load(
// IR-NOT: call void (...) @__ptsan_loop_preanalysis(
// IR: call void (...) @__ptsan_access({{.*}}<16 x i1>
// IR-NEXT: {{.*}}call <16 x i32> @llvm.masked.load.v16i32.p0(

// PREANALYZED: Found bad PREANALYZED_LOOP access
// VECTOR: Found bad VECTOR access
// GOOD-NOT: Found bad

#include <immintrin.h>
#include <stdlib.h>

static volatile int Sink;
static volatile unsigned RuntimeMask = 0x000f;

__attribute__((noinline)) static void constant_mask_load(const int *p, int n) {
  __m512i sum = _mm512_setzero_si512();
  for (int i = 0; i < n; ++i) {
    __m512i value = _mm512_maskz_loadu_epi32(0x000c, p + i * 16);
    sum = _mm512_add_epi32(sum, value);
  }
  Sink = _mm512_cvtsi512_si32(sum);
}

__attribute__((noinline)) static void runtime_mask_load(const int *p, int n,
                                                        __mmask16 mask) {
  __m512i sum = _mm512_setzero_si512();
  for (int i = 0; i < n; ++i) {
    __m512i value = _mm512_maskz_loadu_epi32(mask, p + i * 16);
    sum = _mm512_add_epi32(sum, value);
  }
  Sink = _mm512_cvtsi512_si32(sum);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    return 1;
  }

  int bad = atoi(argv[2]);
  int n = atoi(argv[3]);
  int count = bad ? 3 : 4;
  int *p = (int *)malloc((size_t)count * sizeof(int));
  if (!p) {
    return 1;
  }
  for (int i = 0; i < count; ++i) {
    p[i] = i + 1;
  }

  if (argv[1][0] == 'c') {
    constant_mask_load(p, n);
  } else if (argv[1][0] == 'r') {
    runtime_mask_load(p, n, (__mmask16)RuntimeMask);
  } else {
    return 1;
  }

  _Exit(0);
}
