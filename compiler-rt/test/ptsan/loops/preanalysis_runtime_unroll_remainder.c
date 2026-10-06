//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_raw -fsanitize=ptsan -O3 -fno-vectorize -fno-slp-vectorize \
// RUN:   -fno-discard-value-names -mllvm -ptsan-stop-after=optimization \
// RUN:   -S -emit-llvm -o %t.ll %s
// RUN: FileCheck %s < %t.ll
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-vectorize -fno-slp-vectorize \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 -o test %s
// RUN: ./test 2> good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: not ./test bad 2> bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt
//
// Runtime unrolling can move a scalar remainder outside the surviving loop.
// Loop preanalysis records the original preheader before that transformation.
// The resulting precheck must remain there and cover both the escaped remainder
// store and the stores left in the transformed loop.

__attribute__((noinline))
void runtime_unroll_remainder(int *array, unsigned count) {
#pragma clang loop unroll_count(2)
  for (unsigned i = 0; i < count; ++i) {
    array[i] = (int)i;
  }
}

int main(int argc, char **argv) {
  int array[2] = {0};
  unsigned count = argc > 1 ? 3 : 1;
  runtime_unroll_remainder(array, count);
  return 0;
}

// There is one preanalysis check before the unrolled-loop/remainder split.  The
// marker call identifies its old preheader and remains until PTSan lowering.
// Neither the scalar remainder nor the surviving loop stores retain an
// individual access anchor.
//
// CHECK-LABEL: define {{.*}} @runtime_unroll_remainder(
// CHECK: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 [[LOOP_ID:[0-9]+]]), "deopt"(ptr %array, i32 %count) ], !ptsan.loop.bounds ![[BOUNDS:[0-9]+]]
// CHECK-COUNT-1: call void (...) @__ptsan_loop_preanalysis(
// CHECK: br i1 {{.*}}, label %for.cond.cleanup, label %for.body.epil
// CHECK: for.body.epil:
// CHECK-NOT: call void (...) @__ptsan_access(
// CHECK: store i32
// CHECK: for.body:
// CHECK-NOT: call void (...) @__ptsan_access(
// CHECK: store i32
// CHECK: ![[BOUNDS]] = !{!"ptsan.loop.bounds", i1 true, {{.*}}}

// GOOD-NOT: Found bad
// BAD: Found bad {{.*}} access
