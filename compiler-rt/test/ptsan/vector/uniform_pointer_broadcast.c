//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O3 -march=skylake-avx512 -DNDEBUG \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-stop-after=discovery -fno-discard-value-names \
// RUN:   -S -emit-llvm -o discovery.ll %s
// RUN: FileCheck %s --check-prefix=DISCOVERY < discovery.ll
// RUN: %clang_ptsan -O3 -march=skylake-avx512 -DNDEBUG \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -mllvm -ptsan-abort-on-error -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe inactive 2>inactive.txt
// RUN: FileCheck %s --check-prefix=GOOD < inactive.txt
// RUN: not ./%exe bad 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// This is the pointer shape responsible for CoMD's redundant bounds gathers.
// Ordinary loop vectorization broadcasts the address of a loop-invariant
// structure field and uses it in a masked gather. Every lane of that broadcast
// has one allocation ID, so discovery must retain a scalar ID and scalar bounds
// loads instead of creating bounds-table gathers.

// DISCOVERY-LABEL: define {{.*}} @masked_uniform(
// DISCOVERY: [[FIELD_INSERT:%.*]] = insertelement <4 x ptr> poison, ptr {{%.*}}, i64 0
// DISCOVERY: [[FIELD_SPLAT:%.*]] = shufflevector <4 x ptr> [[FIELD_INSERT]], <4 x ptr> poison, <4 x i32> zeroinitializer
// DISCOVERY: [[RAW_FIELD:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[FIELD_SPLAT]])
// DISCOVERY: [[BOXES_ID:%.*]] = call i64 @__ptsan_get_id(ptr %boxes)
// DISCOVERY: call i64 @__ptsan_load_base(i64 [[BOXES_ID]])
// DISCOVERY: call i64 @__ptsan_load_span(i64 [[BOXES_ID]])
// DISCOVERY-NOT: call <4 x i64> @__ptsan_get_id
// DISCOVERY-NOT: call <4 x i64> @__ptsan_load_base
// DISCOVERY-NOT: call <4 x i64> @__ptsan_load_span
// DISCOVERY: {{.*}}call <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr> [[RAW_FIELD]],
// DISCOVERY: ret void

// GOOD-NOT: Found bad
// BAD: Found bad VECTOR access

#include <stdlib.h>
#include <string.h>

typedef struct {
  int grid_size[3];
  int local_boxes;
  int total_boxes;
} Grid;

__attribute__((noinline)) void
masked_uniform(Grid *boxes, const int *restrict active, int *restrict output,
               int n) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < n; ++i)
    if (active[i])
      output[i] = boxes->local_boxes;
}

int main(int argc, char **argv) {
  Grid boxes;
  int active[4] = {1, 1, 1, 1};
  int output[4] = {-1, -1, -1, -1};
  memset(&boxes, 0, sizeof(boxes));
  boxes.local_boxes = 64;

  Grid *selected = argc > 1 ? &boxes + 1 : &boxes;
  if (argc > 1 && argv[1][0] == 'i')
    memset(active, 0, sizeof(active));
  masked_uniform(selected, active, output, 4);

  for (int i = 0; i < 4; ++i) {
    int expected = active[i] ? 64 : -1;
    if (output[i] != expected)
      return 1;
  }
  return 0;
}
