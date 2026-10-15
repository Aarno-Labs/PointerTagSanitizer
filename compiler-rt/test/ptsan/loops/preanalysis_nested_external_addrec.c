//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -fsanitize=ptsan -O2 -fno-discard-value-names \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-disable \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s \
// RUN:   --implicit-check-not=ptsan.value.id
// RUN: %clang_raw -fsanitize=ptsan -O3 -fno-discard-value-names \
// RUN:   -mllvm -ptsan-stop-after=optimization \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=PLACEMENT
//
// Model CoMD's nested box/atom loop: the inner trip count is loaded separately
// for each outer iteration, may be zero, and the array base is loaded only on
// the path entering the inner loop.  The store therefore cannot be covered by
// a check in the outer preheader.  For the inner loop, however, the access is
// affine.  PTSan resolves the inner recurrence but retains the outer recurrence
// as an inner-loop-invariant component of the accessed range.  Preanalysis
// serializes that component in terms of the dominating row-offset SSA value.

struct nested_data {
  int *array;
  const unsigned *counts;
};

__attribute__((noinline))
void nested_external_addrec(struct nested_data *data, unsigned rows) {
  unsigned row_offset = 0;
  for (unsigned row = 0; row < rows; ++row, row_offset += 64) {
    unsigned columns = data->counts[row];
    for (unsigned column = 0; column < columns; ++column) {
      data->array[row_offset + column] = (int)column;
    }
  }
}

// The important optimized IR is equivalent to:
//
// outer:
//   %row_offset = phi i32 [ 0, %entry ], [ %next_row_offset, %outer_latch ]
//   %columns = load i32, ptr %count_ptr
//   br i1 %columns_is_zero, label %outer_latch, label %inner_preheader
//
// inner_preheader:
//   %array = load ptr, ptr %data
//   br label %inner
//
// inner:
//   %column = phi i64 [ 0, %inner_preheader ], [ %next_column, %inner ]
//   %column32 = trunc i64 %column to i32
//   %index32 = add i32 %row_offset, %column32
//   %index = zext i32 %index32 to i64
//   %element = getelementptr inbounds i32, ptr %array, i64 %index
//   store i32 ..., ptr %element
//
// ScalarEvolution consequently describes %index as the nested recurrence
//
//   {{0,+,64}<outer>,+,1}<inner>
//
// and its value on the final inner iteration as
//
//   ({-1,+,64}<outer> + zext(%columns)).
//
// The -1 is the ordinary last-access correction: an inner loop with `columns`
// iterations last accesses column `columns - 1`.  SCEV folds that -1 into the
// start of the enclosing recurrence.  `%row_offset` is an exact IR
// representative of `{0,+,64}<outer>`, but there is no instruction whose SCEV
// is the shifted `{-1,+,64}<outer>` recurrence.

// The outer loop remains incomplete: it can cover the counts load, but not the
// conditionally loaded array base/store.  The inner loop retains its own
// complete, non-empty preanalysis summary.
// CHECK-LABEL: define {{.*}} @nested_external_addrec(
// CHECK: [[ROW:%row_offset[^ ]*]] = phi i32
// CHECK: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"(ptr {{.*}}, i32 [[ROW]], i32 {{.*}}) ], {{.*}}!ptsan.loop.bounds ![[BOUNDS:[0-9]+]]
// CHECK: br i1 {{.*}}, label {{.*}}, label {{.*}}, !llvm.loop ![[OUTER:[0-9]+]]
// CHECK: store i32
// CHECK: br i1 {{.*}}, label {{.*}}, label {{.*}}, !llvm.loop ![[INNER:[0-9]+]]
// CHECK-DAG: !{!"ptsan.loop.bounds", i1 false, !
// CHECK-DAG: ![[BOUNDS]] = !{!"ptsan.loop.bounds", i1 true, ![[BASE:[0-9]+]]}
// CHECK-DAG: ![[BASE]] = !{!"base", {{![0-9]+}}, ![[RANGE:[0-9]+]]}
// CHECK-DAG: ![[RANGE]] = !{!"LoopAccessRange", ![[MIN:[0-9]+]], {{.*}}}
// CHECK-DAG: ![[MIN]] = !{!"scev", i16 5, ![[ROW_TERM:[0-9]+]], {{.*}}}
// CHECK-DAG: ![[ROW_TERM]] = !{!"scev", i16 6, {{.*}}![[ROW_ADDREC:[0-9]+]]}
// CHECK-DAG: ![[ROW_ADDREC]] = !{!"scev", i16 3, ![[ROW_UNKNOWN:[0-9]+]], i32 64}
// CHECK-DAG: ![[ROW_UNKNOWN]] = !{!"scev", i16 15, ![[ROW_CAPTURE:[0-9]+]]}
// CHECK-DAG: ![[ROW_CAPTURE]] = !{!"capture", i32 1}

// Runtime unrolling moves a scalar prologue in front of the surviving inner
// loop.  The inner precheck remains at the original preheader, before that
// split, and the escaped prologue store is elided through its loop-origin tag.
// The other precheck belongs to the outer loop.
//
// PLACEMENT-LABEL: define {{.*}} @nested_external_addrec(
// PLACEMENT: call void @llvm.sideeffect()
// PLACEMENT: call void (...) @__ptsan_loop_preanalysis(
// PLACEMENT: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"({{.*}}) ], {{.*}}!ptsan.loop.bounds ![[INNER_BOUNDS:[0-9]+]]
// PLACEMENT: call void (...) @__ptsan_loop_preanalysis(
// PLACEMENT: for.body4.prol:
// PLACEMENT-NOT: call void (...) @__ptsan_access(
// PLACEMENT: store i32 {{.*}}, !ptsan.loop.ids ![[INNER_ORIGINS:[0-9]+]]
// PLACEMENT: for.body4:
// PLACEMENT-NOT: call void (...) @__ptsan_access(
// PLACEMENT: store i32 {{.*}}, !ptsan.loop.ids ![[INNER_ORIGINS]]
// PLACEMENT: ![[INNER_BOUNDS]] = !{!"ptsan.loop.bounds", i1 true, !
