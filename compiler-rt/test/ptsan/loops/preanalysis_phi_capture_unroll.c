//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-vectorize -fno-slp-vectorize \
// RUN:   -fno-discard-value-names -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-abort-on-error -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 -o no-preanalysis %s
// RUN: ./no-preanalysis good 2> no-preanalysis-good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < no-preanalysis-good.txt
// RUN: not ./no-preanalysis bad 2> no-preanalysis-bad.txt
// RUN: FileCheck %s --check-prefix=BAD < no-preanalysis-bad.txt
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-vectorize -fno-slp-vectorize \
// RUN:   -fno-discard-value-names -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-abort-on-error -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=PREANALYSIS
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-vectorize -fno-slp-vectorize \
// RUN:   -fno-discard-value-names -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-abort-on-error -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -o preanalysis %s
// RUN: ./preanalysis good 2> preanalysis-good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < preanalysis-good.txt
// RUN: not ./preanalysis bad 2> preanalysis-bad.txt
// RUN: FileCheck %s --check-prefix=BAD < preanalysis-bad.txt

// GOOD-NOT: Found bad
// BAD: Found bad

// PREANALYSIS-LABEL: define {{.*}} @read_rows(
// PREANALYSIS: [[ROW:%row_offset[^ ]*]] = phi i32
// PREANALYSIS: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 [[LOOP_ID:[0-9]+]]), "deopt"({{.*}}i32 [[ROW]]{{.*}}) ]{{.*}}!ptsan.loop.bounds
// PREANALYSIS: call void (...) @__ptsan_loop_preanalysis(
// PREANALYSIS: for.body5.us:
// PREANALYSIS-NOT: call void (...) @__ptsan_access(
// PREANALYSIS: [[NEXT:%add10[^ ]*]] = add i32
// PREANALYSIS: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 [[LOOP_ID]]), "deopt"({{.*}}i32 [[NEXT]]{{.*}}) ]{{.*}}!ptsan.loop.bounds
// PREANALYSIS: call void (...) @__ptsan_loop_preanalysis(
// PREANALYSIS: for.body5.us.1:
// PREANALYSIS-NOT: call void (...) @__ptsan_access(
// PREANALYSIS: br i1

// A runtime regression for an inner-loop summary serialized through an
// enclosing induction PHI.  The volatile increment makes the outer access
// range unanalyzable, but the inner loop can still describe its range in terms
// of the current value of row_offset. That value must be captured at the
// original inner-loop preheader.
//
// Late outer-loop unrolling makes two copies of the inner loop. The first uses
// the surviving row_offset PHI; the second uses the value produced by the first
// copy's increment. Both copies retain the original inner-loop metadata, but
// cloning remaps the preheader marker's captured row_offset operand. The two
// copies therefore have different capture environments for the same summary.
//
// With preanalysis disabled, "good" accesses array[0] and array[63], while
// "bad" accesses array[0] and the one-past element array[64]. PTSan accepts
// the former and reports the latter. The intended multi-environment support
// materializes the summary separately at both markers and removes both
// ordinary inner-loop checks. The runtime runs preserve the good/bad soundness
// requirement.

#include <string.h>

#define ARRAY_SIZE 64

volatile unsigned runtime_columns = 1;
volatile unsigned runtime_rows = 2;
volatile int result_sink;

__attribute__((noinline))
int read_rows(int *array, const volatile unsigned *increments, unsigned rows,
              unsigned columns) {
  __builtin_assume((rows & 1) == 0);

  unsigned row_offset = 0;
  int sum = 0;
#pragma clang loop vectorize(disable) unroll_count(2)
  // The volatile update prevents preanalysis from expressing the array
  // accesses as one range over the outer loop, while the fixed inner loop can
  // still be summarized in terms of the current outer induction PHI.
  for (unsigned row = 0; row < rows;
       row_offset += increments[row], ++row) {
#pragma clang loop vectorize(disable) unroll(disable)
    for (unsigned column = 0; column < columns; ++column) {
      sum += array[row_offset + column];
    }
  }
  return sum;
}

int main(int argc, char **argv) {
  int array[ARRAY_SIZE];
  volatile unsigned increments[4] = {
      argc > 1 && argv[1][0] == 'b' ? ARRAY_SIZE : ARRAY_SIZE - 1, 0, 0, 0};

  memset(array, 0, sizeof(array));

  // "good" reads array[0] and array[63].  "bad" reads array[0] and the
  // one-past element array[64].
  result_sink = read_rows(array, increments, runtime_rows, runtime_columns);
  return 0;
}
