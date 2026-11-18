//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O3 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O3 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 %s -o %t
// RUN: %run %t 64 > %t.good 2>&1
// RUN: FileCheck %s --check-prefix=GOOD < %t.good
// RUN: not %run %t 65 > %t.bad 2>&1
// RUN: FileCheck %s --check-prefix=BAD < %t.bad

// Static-bound folding tags the global base. Materializing the inner loop's
// range then casts the global pointer to an integer. LLVM 23 folds truncation
// into a narrow ptrtoint, which can need a fresh anchor. Late optimization must
// preserve tagged polarity without nesting one TaggedAnchor inside another.
// Keep the loops scalar so the range begins at the global, not a remainder GEP.
//
// IR-LABEL: define {{.*}}@sum(
// IR: [[GLOBAL:%.*]] = load ptr, ptr @values.ptsan.ptr
// IR: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr returned [[GLOBAL]])
// IR: call i64 @__ptsan_static_base(ptr [[TAGGED]])
// IR-NOT: call ptr @__ptsan_tagged(ptr returned [[TAGGED]])
// IR: [[FIRST_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr returned [[GLOBAL]])
// IR-NEXT: [[LOW:%.*]] = ptrtoint ptr [[FIRST_TAGGED]] to i48
// IR-NEXT: %ptsan.first.low = zext i48 [[LOW]] to i64
// GOOD-NOT: Found bad
// GOOD: sum: 43680
// GOOD-NOT: Found bad
// BAD: Found bad LOOP access

#include <stdio.h>
#include <stdlib.h>

short values[64];

__attribute__((noinline)) int sum(unsigned count) {
  int result = 0;
  for (unsigned j = 0; j < count; ++j) {
    result += values[j];
    for (unsigned i = 0; i < j; ++i)
      if (values[i] < values[j])
        result += values[i];
  }
  return result;
}

int main(int argc, char **argv) {
  for (int i = 0; i < 64; ++i)
    values[i] = i;
  unsigned count = argc > 1 ? atoi(argv[1]) : 64;
  printf("sum: %d\n", sum(count));
  return 0;
}
