//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Loop preanalysis metadata may depend on an SSA load value. Loads are memory
// reads and must be captured at the original preheader rather than
// reconstructed from pointer/type operands.
//
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-disable \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s \
// RUN:   --implicit-check-not=ptsan.value.id
//
// CHECK-LABEL: define {{.*}}@copy_loaded_bound(
// CHECK: [[SIZE:%.*]] = load i32, ptr {{.*}}, align 4
// CHECK: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"({{.*}}i32 [[SIZE]]{{.*}}) ], !ptsan.loop.bounds ![[BOUNDS:[0-9]+]]
// CHECK: ![[BOUNDS]] = !{!"ptsan.loop.bounds", i1 {{.*}}, !
// CHECK: !{!"capture", i32 1}

#include <stdint.h>

unsigned sz;

__attribute__((noinline)) void copy_loaded_bound(uint8_t *dst, uint8_t *src) {
  unsigned local_sz = sz;
  for (unsigned i = 0; i < local_sz; ++i) {
    dst[i] = src[i];
  }
}
