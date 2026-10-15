//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -march=skylake-avx512 \
// RUN:   -fno-discard-value-names -mllvm -unroll-count=2 \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-stop-after=optimization \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=CAPTURE
// RUN: %clang_raw -fsanitize=ptsan -g -O3 \
// RUN:   -fno-discard-value-names -mllvm -unroll-count=2 \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 -o test %s
// RUN: ./test 2> good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: not ./test bad 2> bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// CAPTURE-LABEL: define {{.*}} @read_rows(
// CAPTURE: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"(ptr %rows, i32 %row_count) ]{{.*}}!ptsan.loop.bounds
// CAPTURE: br i1 {{.*}}, label %[[REMAINDER_DISPATCH:[^, ]+]], label %[[MAIN_ENTRY:[^, ]+]]
// CAPTURE: {{^}}[[MAIN_ENTRY]]:
// CAPTURE: [[FIRST_BASE:%.*]] = load ptr, ptr
// CAPTURE: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 [[INNER_ID:[0-9]+]]), "deopt"(ptr [[FIRST_BASE]], i32 %column_count) ]{{.*}}!ptsan.loop.bounds
// CAPTURE-NEXT: call void (...) @__ptsan_loop_preanalysis(
// CAPTURE-NOT: call void (...) @__ptsan_access(
// CAPTURE: [[SECOND_BASE:%.*]] = load ptr, ptr
// CAPTURE: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 [[INNER_ID]]), "deopt"(ptr [[SECOND_BASE]], i32 %column_count) ]{{.*}}!ptsan.loop.bounds
// CAPTURE-NEXT: call void (...) @__ptsan_loop_preanalysis(
// CAPTURE-NOT: call void (...) @__ptsan_access(
// CAPTURE: {{^}}[[REMAINDER_DISPATCH]]:
// CAPTURE: br i1 {{.*}}, label %for.cond.cleanup, label %[[REMAINDER:[^, ]+]]
// CAPTURE: {{^}}[[REMAINDER]]:
// CAPTURE: [[REMAINDER_BASE:%.*]] = load ptr, ptr
// CAPTURE: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 [[INNER_ID]]), "deopt"(ptr [[REMAINDER_BASE]], i32 %column_count) ]{{.*}}!ptsan.loop.bounds
// CAPTURE-NEXT: call void (...) @__ptsan_loop_preanalysis(
// CAPTURE-NOT: call void (...) @__ptsan_access(
// CAPTURE: ret i32

// GOOD-NOT: Found bad
// BAD: Found bad {{.*}} access

#include <string.h>

#define ROWS 2
#define COLUMNS 1

volatile int preanalysis_base_identity_sink;
volatile unsigned preanalysis_base_identity_rows = ROWS;
volatile unsigned preanalysis_base_identity_columns = COLUMNS;

// The inner loop's base is an opaque pointer loaded in the outer loop. Outer
// unrolling clones both that load and the inner preheader marker. The two main
// loop markers are sequential, but the runtime remainder is reached either
// directly or after the main loop. Its marker and the main-loop markers are
// therefore incomparable. All three retain the same inner-loop ID while
// capturing distinct pointer-producing SSA values; a multi-environment
// representation cannot require pointer-identical deserialized bases or a
// single marker which dominates every access. The runtime half makes this a
// soundness test as well as an IR-shape test.
__attribute__((noinline))
static int read_rows(int *const *rows, unsigned row_count,
                     unsigned column_count) {
  int sum = 0;

#pragma clang loop vectorize(disable) unroll_count(2)
  for (unsigned row = 0; row < row_count; ++row) {
    int *base = rows[row];

#pragma clang loop vectorize(disable) unroll(disable)
    for (unsigned column = 0; column < column_count; ++column) {
      sum += base[column];
    }
  }

  return sum;
}

int main(int argc, char **argv) {
  int first[COLUMNS];
  int second[COLUMNS];
  int *rows[ROWS] = {first, second};

  memset(first, 0, sizeof(first));
  memset(second, 0, sizeof(second));
  if (argc > 1 && argv[1][0] == 'b') {
    rows[1] = second + COLUMNS;
  }

  preanalysis_base_identity_sink =
      read_rows(rows, preanalysis_base_identity_rows,
                preanalysis_base_identity_columns);
  return 0;
}
