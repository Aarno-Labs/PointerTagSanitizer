;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: split-file %s %t
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=0 -ptsan-stop-after=late-optimization %t/scalar-id16.ll | FileCheck %t/scalar-id16.ll --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=0 %t/scalar-id16.ll -S -o /dev/null
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=0 -ptsan-stop-after=late-optimization %t/v2-id16.ll | FileCheck %t/v2-id16.ll --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=0 %t/v2-id16.ll -S -o /dev/null

; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 -ptsan-stop-after=late-optimization %t/scalar-id32.ll | FileCheck %t/scalar-id32.ll --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 %t/scalar-id32.ll -S -o /dev/null
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 -ptsan-stop-after=late-optimization %t/v2-id32.ll | FileCheck %t/v2-id32.ll --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 %t/v2-id32.ll -S -o /dev/null
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=0 -ptsan-stop-after=late-optimization %t/v4.ll | FileCheck %t/v4.ll --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 -ptsan-stop-after=late-optimization %t/v4.ll | FileCheck %t/v4.ll --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=0 %t/v4.ll -o /dev/null
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 %t/v4.ll -o /dev/null

; The shared vector input uses ordinary scatters to create stripped demands.
; The separate scalar and widening-shuffle inputs deliberately start with stripped
; values: their stores alone would not cause discovery to create those strips.

;--- v4.ll
; Global strip-ID hoisting keeps a scalar common ID while walking through a
; scalar-base/vector-index GEP and a whole-vector PHI. The ID is widened only
; when a later lane-wise operation actually requires a vector.
;
; A vector-base GEP instead preserves one independent ID per pointer lane.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @llvm.masked.scatter.v4i8.v4p0(<4 x i8>, <4 x ptr>, i32 immarg, <4 x i1>)


define void @common_id(ptr %base, ptr %destination, i1 %done) {
; LATE-LABEL: define void @common_id(
; LATE-DAG: [[RAW_BASE:%.*]] = call ptr @__ptsan_strip{{(_id32)?}}(ptr %base
; LATE-DAG: [[BASE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %base)
; LATE: %pointers = getelementptr inbounds i8, ptr [[RAW_BASE]], <4 x i64> %splat
; LATE: %last = phi <4 x ptr> [ %pointers, %loop ]
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag{{(_id32)?}}(<4 x ptr> %last, i64 [[BASE_ID]]
; LATE: call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
entry:
  br label %loop

loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  %indices = insertelement <4 x i64> poison, i64 %index, i64 0
  %splat = shufflevector <4 x i64> %indices, <4 x i64> poison,
      <4 x i32> zeroinitializer
  %pointers = getelementptr inbounds i8, ptr %base, <4 x i64> %splat
  call void @llvm.masked.scatter.v4i8.v4p0(<4 x i8> zeroinitializer,
      <4 x ptr> %pointers, i32 1, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  %next = add i64 %index, 1
  br i1 %done, label %exit, label %loop

exit:
  %last = phi <4 x ptr> [ %pointers, %loop ]
  store <4 x ptr> %last, ptr %destination, align 8
  ret void
}

define void @lane_ids(<4 x ptr> %bases, ptr %destination, i1 %done) {
; LATE-LABEL: define void @lane_ids(
; LATE-DAG: [[RAW_BASES:%.*]] = call <4 x ptr> @__ptsan_strip{{(_id32)?}}(<4 x ptr> %bases
; LATE-DAG: [[BASE_IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> %bases)
; LATE: %pointers = getelementptr inbounds i8, <4 x ptr> [[RAW_BASES]], i64 %index
; LATE: %last = phi <4 x ptr> [ %pointers, %loop ]
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag{{(_id32)?}}(<4 x ptr> %last, <4 x i64> [[BASE_IDS]]
; LATE: call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
entry:
  br label %loop

loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  %pointers = getelementptr inbounds i8, <4 x ptr> %bases, i64 %index
  call void @llvm.masked.scatter.v4i8.v4p0(<4 x i8> zeroinitializer,
      <4 x ptr> %pointers, i32 1, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  %next = add i64 %index, 1
  br i1 %done, label %exit, label %loop

exit:
  %last = phi <4 x ptr> [ %pointers, %loop ]
  store <4 x ptr> %last, ptr %destination, align 8
  ret void
}

; A general pointer shuffle remains lane-wise. When global hoisting strips the
; shuffle, retagging mirrors its mask in the two vector ID operands.
define void @shuffled_lane_ids(<4 x ptr> %lhs, <4 x ptr> %rhs,
                               ptr %destination, i1 %inner_done,
                               i1 %outer_done, i1 %choose_lhs) {
; LATE-LABEL: define void @shuffled_lane_ids(
; LATE-DAG: [[RAW_LHS:%.*]] = call <4 x ptr> @__ptsan_strip{{(_id32)?}}(<4 x ptr> %lhs
; LATE-DAG: [[RAW_RHS:%.*]] = call <4 x ptr> @__ptsan_strip{{(_id32)?}}(<4 x ptr> %rhs
; LATE-DAG: [[LHS_IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> %lhs)
; LATE-DAG: [[RHS_IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> %rhs)
; LATE: %selected = select i1 %choose_lhs, <4 x ptr> [[RAW_LHS]], <4 x ptr> [[RAW_RHS]]
; LATE-NEXT: [[SELECTED_IDS:%.*]] = select i1 %choose_lhs, <4 x i64> [[LHS_IDS]], <4 x i64> [[RHS_IDS]]
; LATE: %shuffled = shufflevector <4 x ptr> %selected, <4 x ptr> [[RAW_RHS]], <4 x i32> <i32 0, i32 5, i32 2, i32 7>
; LATE-NEXT: [[SHUFFLED_IDS:%.*]] = shufflevector <4 x i64> [[SELECTED_IDS]], <4 x i64> [[RHS_IDS]], <4 x i32> <i32 0, i32 5, i32 2, i32 7>
; LATE: %last = phi <4 x ptr> [ %outer_last, %outer_latch ]
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag{{(_id32)?}}(<4 x ptr> %last, <4 x i64> [[SHUFFLED_IDS]]
; LATE: call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
entry:
  br label %outer

outer:
  br label %inner

inner:
  %selected = select i1 %choose_lhs, <4 x ptr> %lhs, <4 x ptr> %rhs
  %shuffled = shufflevector <4 x ptr> %selected, <4 x ptr> %rhs,
      <4 x i32> <i32 0, i32 5, i32 2, i32 7>
  call void @llvm.masked.scatter.v4i8.v4p0(<4 x i8> zeroinitializer,
      <4 x ptr> %shuffled, i32 1, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  br i1 %inner_done, label %outer_latch, label %inner

outer_latch:
  %outer_last = phi <4 x ptr> [ %shuffled, %inner ]
  br i1 %outer_done, label %exit, label %outer

exit:
  %last = phi <4 x ptr> [ %outer_last, %outer_latch ]
  store <4 x ptr> %last, ptr %destination, align 8
  ret void
}

; A lane-wise select mixing a scalar-common ID with per-lane IDs widens only
; the scalar arm. The condition and resulting ID select remain vector-shaped.
define void @selected_lane_ids(ptr %common, <4 x ptr> %lanes,
                               <4 x i64> %offsets, <4 x i1> %choose_common,
                               ptr %destination, i1 %inner_done,
                               i1 %outer_done) {
; LATE-LABEL: define void @selected_lane_ids(
; LATE-DAG: [[RAW_COMMON:%.*]] = call ptr @__ptsan_strip{{(_id32)?}}(ptr %common
; LATE-DAG: [[RAW_LANES:%.*]] = call <4 x ptr> @__ptsan_strip{{(_id32)?}}(<4 x ptr> %lanes
; LATE-DAG: [[COMMON_ID:%.*]] = call i64 @__ptsan_get_id(ptr %common)
; LATE-DAG: [[LANE_IDS:%.*]] = call <4 x i64> @__ptsan_get_id(<4 x ptr> %lanes)
; LATE-DAG: [[COMMON_ID_INSERT:%.*]] = insertelement <4 x i64> poison, i64 [[COMMON_ID]], i64 0
; LATE-DAG: [[COMMON_IDS:%.*]] = shufflevector <4 x i64> [[COMMON_ID_INSERT]], <4 x i64> poison, <4 x i32> zeroinitializer
; LATE: %common_ptrs = getelementptr inbounds i8, ptr [[RAW_COMMON]], <4 x i64> %offsets
; LATE: %lane_ptrs = getelementptr inbounds i8, <4 x ptr> [[RAW_LANES]], <4 x i64> %offsets
; LATE: %selected = select <4 x i1> %choose_common, <4 x ptr> %common_ptrs, <4 x ptr> %lane_ptrs
; LATE-NEXT: [[SELECTED_IDS:%.*]] = select <4 x i1> %choose_common, <4 x i64> [[COMMON_IDS]], <4 x i64> [[LANE_IDS]]
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag{{(_id32)?}}(<4 x ptr> %last, <4 x i64> [[SELECTED_IDS]]
; LATE: call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
entry:
  br label %outer

outer:
  br label %inner

inner:
  %common_ptrs = getelementptr inbounds i8, ptr %common, <4 x i64> %offsets
  %lane_ptrs = getelementptr inbounds i8, <4 x ptr> %lanes, <4 x i64> %offsets
  %selected = select <4 x i1> %choose_common, <4 x ptr> %common_ptrs,
      <4 x ptr> %lane_ptrs
  call void @llvm.masked.scatter.v4i8.v4p0(<4 x i8> zeroinitializer,
      <4 x ptr> %selected, i32 1, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  br i1 %inner_done, label %outer_latch, label %inner

outer_latch:
  %outer_last = phi <4 x ptr> [ %selected, %inner ]
  br i1 %outer_done, label %exit, label %outer

exit:
  %last = phi <4 x ptr> [ %outer_last, %outer_latch ]
  store <4 x ptr> %last, ptr %destination, align 8
  ret void
}

;--- scalar-id16.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr) nofree

; A whole-vector PHI between different scalar bases needs an ID PHI to retain
; the control-flow choice, but the ID itself remains scalar-common.
define void @different_scalar_bases(<4 x i64> %offsets, ptr %lhs, ptr %rhs,
                                    ptr %destination, i1 %choose_lhs) {
; LATE-LABEL: define void @different_scalar_bases(
; LATE-DAG: [[RAW_LHS:%.*]] = call ptr @__ptsan_strip(ptr %lhs
; LATE-DAG: [[RAW_RHS:%.*]] = call ptr @__ptsan_strip(ptr %rhs
; LATE-DAG: [[LHS_ID:%.*]] = call i64 @__ptsan_get_id(ptr %lhs)
; LATE-DAG: [[RHS_ID:%.*]] = call i64 @__ptsan_get_id(ptr %rhs)
; LATE: left:
; LATE: %lhs.pointers = getelementptr inbounds i8, ptr [[RAW_LHS]], <4 x i64> %offsets
; LATE: right:
; LATE: %rhs.pointers = getelementptr inbounds i8, ptr [[RAW_RHS]], <4 x i64> %offsets
; LATE: join:
; LATE-NEXT: [[MERGED_ID:%.*]] = phi i64 [ [[LHS_ID]], %left ], [ [[RHS_ID]], %right ]
; LATE-NEXT: %merged = phi <4 x ptr> [ %lhs.pointers, %left ], [ %rhs.pointers, %right ]
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag(<4 x ptr> %merged, i64 [[MERGED_ID]]
; LATE: call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
entry:
  %lhs.s = call ptr @__ptsan_strip(ptr %lhs)
  %rhs.s = call ptr @__ptsan_strip(ptr %rhs)
  br i1 %choose_lhs, label %left, label %right

left:
  %lhs.pointers = getelementptr inbounds i8, ptr %lhs.s, <4 x i64> %offsets
  br label %join

right:
  %rhs.pointers = getelementptr inbounds i8, ptr %rhs.s, <4 x i64> %offsets
  br label %join

join:
  %merged = phi <4 x ptr> [ %lhs.pointers, %left ],
                           [ %rhs.pointers, %right ]
  store <4 x ptr> %merged, ptr %destination, align 8
  ret void
}

;--- scalar-id32.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip_id32(ptr, i64, i64) nofree
declare i64 @__ptsan_get_id(ptr) nofree
declare i64 @__ptsan_load_base(i64) nofree

; A whole-vector PHI between different scalar bases needs an ID PHI to retain
; the control-flow choice, but the ID itself remains scalar-common.
define void @different_scalar_bases(<4 x i64> %offsets, ptr %lhs, ptr %rhs,
                                    ptr %destination, i1 %choose_lhs) {
; LATE-LABEL: define void @different_scalar_bases(
; LATE-DAG: [[LHS_ID:%.*]] = call i64 @__ptsan_get_id(ptr %lhs)
; LATE-DAG: [[RHS_ID:%.*]] = call i64 @__ptsan_get_id(ptr %rhs)
; LATE: left:
; LATE: [[RAW_LHS:%.*]] = call ptr @__ptsan_strip_id32(ptr %lhs, i64 [[LHS_ID]], i64 {{%.*}})
; LATE: %lhs.pointers = getelementptr inbounds i8, ptr [[RAW_LHS]], <4 x i64> %offsets
; LATE: right:
; LATE: [[RAW_RHS:%.*]] = call ptr @__ptsan_strip_id32(ptr %rhs, i64 [[RHS_ID]], i64 {{%.*}})
; LATE: %rhs.pointers = getelementptr inbounds i8, ptr [[RAW_RHS]], <4 x i64> %offsets
; LATE: join:
; LATE-NEXT: [[MERGED_ID:%.*]] = phi i64 [ [[LHS_ID]], %left ], [ [[RHS_ID]], %right ]
; LATE-NEXT: %merged = phi <4 x ptr> [ %lhs.pointers, %left ], [ %rhs.pointers, %right ]
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag_id32(<4 x ptr> %merged, i64 [[MERGED_ID]]
; LATE: call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
entry:
  %lhs.s.id = call i64 @__ptsan_get_id(ptr %lhs)
  %lhs.s.base = call i64 @__ptsan_load_base(i64 %lhs.s.id)
  %lhs.s = call ptr @__ptsan_strip_id32(ptr %lhs, i64 %lhs.s.id, i64 %lhs.s.base)
  %rhs.s.id = call i64 @__ptsan_get_id(ptr %rhs)
  %rhs.s.base = call i64 @__ptsan_load_base(i64 %rhs.s.id)
  %rhs.s = call ptr @__ptsan_strip_id32(ptr %rhs, i64 %rhs.s.id, i64 %rhs.s.base)
  br i1 %choose_lhs, label %left, label %right

left:
  %lhs.pointers = getelementptr inbounds i8, ptr %lhs.s, <4 x i64> %offsets
  br label %join

right:
  %rhs.pointers = getelementptr inbounds i8, ptr %rhs.s, <4 x i64> %offsets
  br label %join

join:
  %merged = phi <4 x ptr> [ %lhs.pointers, %left ],
                           [ %rhs.pointers, %right ]
  store <4 x ptr> %merged, ptr %destination, align 8
  ret void
}

;--- v2-id16.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <2 x ptr> @__ptsan_strip(<2 x ptr>) nofree

; A shuffle's mask indexes lanes in its operand vectors, even when the result
; has a different width. Its reconstructed ID shuffle must retain that operand
; width so the mask continues to select the same allocation IDs.
define void @widening_shuffle(<2 x ptr> %lhs, <2 x ptr> %rhs,
                              ptr %destination) {
; LATE-LABEL: define void @widening_shuffle(
; LATE-DAG: [[RAW_LHS:%.*]] = call <2 x ptr> @__ptsan_strip(<2 x ptr> %lhs
; LATE-DAG: [[RAW_RHS:%.*]] = call <2 x ptr> @__ptsan_strip(<2 x ptr> %rhs
; LATE-DAG: [[LHS_IDS:%.*]] = call <2 x i64> @__ptsan_get_id(<2 x ptr> %lhs)
; LATE-DAG: [[RHS_IDS:%.*]] = call <2 x i64> @__ptsan_get_id(<2 x ptr> %rhs)
; LATE: %merged = shufflevector <2 x ptr> [[RAW_LHS]], <2 x ptr> [[RAW_RHS]], <4 x i32> <i32 0, i32 2, i32 1, i32 3>
; LATE-NEXT: [[MERGED_IDS:%.*]] = shufflevector <2 x i64> [[LHS_IDS]], <2 x i64> [[RHS_IDS]], <4 x i32> <i32 0, i32 2, i32 1, i32 3>
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag(<4 x ptr> %merged, <4 x i64> [[MERGED_IDS]]
; LATE-NEXT: [[TAGGED:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
; LATE: store <4 x ptr> [[TAGGED]], ptr
entry:
  %lhs.raw = call <2 x ptr> @__ptsan_strip(<2 x ptr> %lhs)
  %rhs.raw = call <2 x ptr> @__ptsan_strip(<2 x ptr> %rhs)
  %merged = shufflevector <2 x ptr> %lhs.raw, <2 x ptr> %rhs.raw,
      <4 x i32> <i32 0, i32 2, i32 1, i32 3>
  store <4 x ptr> %merged, ptr %destination, align 8
  ret void
}
;--- v2-id32.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <2 x ptr> @__ptsan_strip_id32(<2 x ptr>, <2 x i64>, <2 x i64>) nofree
declare <2 x i64> @__ptsan_get_id(<2 x ptr>) nofree
declare <2 x i64> @__ptsan_load_base(<2 x i64>, <2 x i1>) nofree

; A shuffle's mask indexes lanes in its operand vectors, even when the result
; has a different width. Its reconstructed ID shuffle must retain that operand
; width so the mask continues to select the same allocation IDs.
define void @widening_shuffle(<2 x ptr> %lhs, <2 x ptr> %rhs,
                              ptr %destination) {
; LATE-LABEL: define void @widening_shuffle(
; LATE-DAG: [[RAW_LHS:%.*]] = call <2 x ptr> @__ptsan_strip_id32(<2 x ptr> %lhs
; LATE-DAG: [[RAW_RHS:%.*]] = call <2 x ptr> @__ptsan_strip_id32(<2 x ptr> %rhs
; LATE-DAG: [[LHS_IDS:%.*]] = call <2 x i64> @__ptsan_get_id(<2 x ptr> %lhs)
; LATE-DAG: [[RHS_IDS:%.*]] = call <2 x i64> @__ptsan_get_id(<2 x ptr> %rhs)
; LATE: %merged = shufflevector <2 x ptr> [[RAW_LHS]], <2 x ptr> [[RAW_RHS]], <4 x i32> <i32 0, i32 2, i32 1, i32 3>
; LATE-NEXT: [[MERGED_IDS:%.*]] = shufflevector <2 x i64> [[LHS_IDS]], <2 x i64> [[RHS_IDS]], <4 x i32> <i32 0, i32 2, i32 1, i32 3>
; LATE: [[RETAGGED:%.*]] = call <4 x ptr> @__ptsan_tag_id32(<4 x ptr> %merged, <4 x i64> [[MERGED_IDS]]
; LATE-NEXT: [[TAGGED:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> {{.*}}[[RETAGGED]])
; LATE: store <4 x ptr> [[TAGGED]], ptr
entry:
  %lhs.raw.id = call <2 x i64> @__ptsan_get_id(<2 x ptr> %lhs)
  %lhs.raw.base = call <2 x i64> @__ptsan_load_base(<2 x i64> %lhs.raw.id, <2 x i1> <i1 true, i1 true>)
  %lhs.raw = call <2 x ptr> @__ptsan_strip_id32(<2 x ptr> %lhs, <2 x i64> %lhs.raw.id, <2 x i64> %lhs.raw.base)
  %rhs.raw.id = call <2 x i64> @__ptsan_get_id(<2 x ptr> %rhs)
  %rhs.raw.base = call <2 x i64> @__ptsan_load_base(<2 x i64> %rhs.raw.id, <2 x i1> <i1 true, i1 true>)
  %rhs.raw = call <2 x ptr> @__ptsan_strip_id32(<2 x ptr> %rhs, <2 x i64> %rhs.raw.id, <2 x i64> %rhs.raw.base)
  %merged = shufflevector <2 x ptr> %lhs.raw, <2 x ptr> %rhs.raw,
      <4 x i32> <i32 0, i32 2, i32 1, i32 3>
  store <4 x ptr> %merged, ptr %destination, align 8
  ret void
}
