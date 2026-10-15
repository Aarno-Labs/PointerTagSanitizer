//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_raw -fsanitize=ptsan -O2 -fno-discard-value-names \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-disable \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s \
// RUN:   --implicit-check-not=ptsan.value.id
//
// The innermost access has two enclosing-loop recurrences with separate,
// dominating IR representatives:
//
//   %stride       = {1,+,1}<outer>
//   %middle_offset = {0,+,64}<middle>
//
// For an inner trip count of `columns`, its final accessed index is:
//
//   (columns - 1) * {1,+,1}<outer> + {0,+,64}<middle>
//
// No instruction represents that complete expression.  Representing it in
// terms of %middle_offset leaves an offset that still contains the outer
// AddRec, while representing it in terms of %stride still leaves the middle
// AddRec.  Serializing it therefore requires recursively replacing both
// AddRecs with their respective IR representatives:
//
//   (columns - 1) * Unknown(%stride) + Unknown(%middle_offset)

struct nested_data {
  int *array;
  const unsigned *counts;
};

__attribute__((noinline))
void nested_recursive_representatives(struct nested_data *data,
                                      unsigned outer_rows,
                                      unsigned middle_rows) {
  unsigned stride = 1;
  for (unsigned outer = 0; outer < outer_rows; ++outer, ++stride) {
    unsigned middle_offset = 0;
    for (unsigned middle = 0; middle < middle_rows;
         ++middle, middle_offset += 64) {
      unsigned columns = data->counts[outer * middle_rows + middle];
      for (unsigned column = 0; column < columns; ++column) {
        data->array[middle_offset + column * stride] = (int)column;
      }
    }
  }
}

// CHECK-LABEL: define {{.*}} @nested_recursive_representatives(
// CHECK: [[STRIDE:%stride[^ ]*]] = phi i32
// CHECK: [[MIDDLE:%middle_offset[^ ]*]] = phi i32
// CHECK: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"(ptr {{.*}}, i32 [[MIDDLE]], i32 {{.*}}, i32 [[STRIDE]]) ], {{.*}}!ptsan.loop.bounds ![[BOUNDS:[0-9]+]]
// CHECK: store i32
// CHECK: br i1 {{.*}}, label {{.*}}, label {{.*}}, !llvm.loop ![[INNER:[0-9]+]]
// CHECK-DAG: ![[BOUNDS]] = !{!"ptsan.loop.bounds", i1 true, ![[BASE:[0-9]+]]}
// CHECK-DAG: ![[BASE]] = !{!"base", {{![0-9]+}}, ![[RANGE:[0-9]+]]}
// CHECK-DAG: ![[RANGE]] = !{!"LoopAccessRange", ![[MIN:[0-9]+]], ![[MAX:[0-9]+]], {{.*}}}
// CHECK-DAG: ![[MIN]] = !{!"scev", i16 5, ![[MIDDLE_TERM:[0-9]+]], {{.*}}}
// CHECK-DAG: ![[MIDDLE_TERM]] = !{!"scev", i16 6, {{.*}}![[MIDDLE_ADDREC:[0-9]+]]}
// CHECK-DAG: ![[MIDDLE_ADDREC]] = !{!"scev", i16 3, ![[MIDDLE_UNKNOWN:[0-9]+]], i32 64}
// CHECK-DAG: ![[MIDDLE_UNKNOWN]] = !{!"scev", i16 15, ![[MIDDLE_CAPTURE:[0-9]+]]}
// CHECK-DAG: ![[MIDDLE_CAPTURE]] = !{!"capture", i32 1}
// CHECK-DAG: ![[MAX]] = !{!"scev", i16 5, ![[MAX_TERM:[0-9]+]], {{.*}}}
// CHECK-DAG: ![[MAX_TERM]] = !{!"scev", i16 6, {{.*}}![[MAX_ADDREC:[0-9]+]]}
// CHECK-DAG: ![[MAX_ADDREC]] = !{!"scev", i16 3, ![[STRIDE_OFFSET:[0-9]+]], i32 64}
// CHECK-DAG: ![[STRIDE_OFFSET]] = !{!"scev", i16 5, ![[STRIDE_TERM:[0-9]+]], {{.*}}}
// CHECK-DAG: ![[STRIDE_TERM]] = !{!"scev", i16 6, {{.*}}![[STRIDE_UNKNOWN:[0-9]+]]}
// CHECK-DAG: ![[STRIDE_UNKNOWN]] = !{!"scev", i16 15, ![[STRIDE_CAPTURE:[0-9]+]]}
// CHECK-DAG: ![[STRIDE_CAPTURE]] = !{!"capture", i32 3}
