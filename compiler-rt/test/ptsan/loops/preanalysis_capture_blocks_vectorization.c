//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -fsanitize=ptsan -O3 -march=skylake-avx512 \
// RUN:   -fno-discard-value-names -mllvm -force-vector-width=4 \
// RUN:   -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=VECTORIZED
// RUN: %clang_raw -fsanitize=ptsan -O3 -march=skylake-avx512 \
// RUN:   -fno-discard-value-names -mllvm -force-vector-width=4 \
// RUN:   -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefixes=VECTORIZED,PREANALYSIS

// Loop preanalysis summarizes the read from the local `active` array and
// captures its alloca in the original-preheader marker. Captures use a deopt
// operand bundle, whose pointer operands are nocapture, so that SSA use must not
// make alias analysis treat the alloca as escaping. The loop must vectorize
// into a masked pointer load and masked scatter both with and without
// preanalysis.

#include <stddef.h>
#include <string.h>

__attribute__((noinline))
void local_active_scatter(int *const *restrict pointers,
                          const int *restrict input_active, size_t count) {
  int active[64];
  __builtin_assume(count <= 64);
  memcpy(active, input_active, sizeof(active));

#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (size_t i = 0; i < count; ++i) {
    if (active[i]) {
      *pointers[i] = 1;
    }
  }
}

// VECTORIZED-LABEL: define {{.*}} @local_active_scatter(
// PREANALYSIS: [[ACTIVE:%active]] = alloca [64 x i32]
// PREANALYSIS: [[ACTIVE_SOURCE:%[0-9]+]] = call ptr @__ptsan_source(ptr [[ACTIVE]])
// PREANALYSIS: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"(ptr [[ACTIVE_SOURCE]], i64 %count) ]{{.*}}!ptsan.loop.bounds
// VECTORIZED: call <4 x ptr> @llvm.masked.load.v4p0.p0(
// VECTORIZED: call void @llvm.masked.scatter.v4i32.v4p0(
