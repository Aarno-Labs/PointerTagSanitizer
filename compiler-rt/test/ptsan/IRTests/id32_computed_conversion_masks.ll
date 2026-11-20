;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-reoptimize=false -ptsan-stop-after=late-optimization %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-reoptimize=false %s -o /dev/null
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-reoptimize=false -ptsan-nochecks %s -o /dev/null

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-musl"

; Placement contract: the conversion's metadata load carries its predicate.
; Distinct predicates must not be merged into a load before either one.

declare <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr>, i32 immarg, <2 x i1>, <2 x i32>)
declare void @llvm.masked.scatter.v2i32.v2p0(<2 x i32>, <2 x ptr>, i32 immarg, <2 x i1>)

; CHECK-LABEL: define <2 x i32> @computed_mask(
; CHECK: %mask = icmp sgt
; CHECK: [[BASE:%.*]] = call <2 x i64> @__ptsan_load_base({{.*}}<2 x i1> %mask)
; CHECK: [[RAW:%.*]] = call <2 x ptr> @__ptsan_strip_id32({{.*}}<2 x i64> [[BASE]])
; CHECK: call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> [[RAW]], i32 4, <2 x i1> %mask,
define <2 x i32> @computed_mask(<2 x ptr> %pointers, <2 x i64> %indices) {
  %mask = icmp sgt <2 x i64> %indices, zeroinitializer
  %value = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %pointers, i32 4, <2 x i1> %mask, <2 x i32> zeroinitializer)
  ret <2 x i32> %value
}

; A second predicate depends on the first gather, so it cannot be hoisted to
; form a shared conversion mask for both accesses.
; CHECK-LABEL: define <2 x i32> @sequential_masks(
; CHECK: %first_mask = icmp sgt
; CHECK: call <2 x ptr> @__ptsan_strip_id32(
; CHECK: %first = call <2 x i32> @llvm.masked.gather
; CHECK: %second_mask = icmp sgt
; CHECK: [[SECOND_BASE:%.*]] = call <2 x i64> @__ptsan_load_base({{.*}}<2 x i1> %second_mask)
; CHECK: [[SECOND_RAW:%.*]] = call <2 x ptr> @__ptsan_strip_id32({{.*}}<2 x i64> [[SECOND_BASE]])
; CHECK: call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> [[SECOND_RAW]], i32 4, <2 x i1> %second_mask,
define <2 x i32> @sequential_masks(<2 x ptr> %pointers, <2 x i64> %indices) {
  %first_mask = icmp sgt <2 x i64> %indices, zeroinitializer
  %first = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %pointers, i32 4, <2 x i1> %first_mask, <2 x i32> zeroinitializer)
  %second_mask = icmp sgt <2 x i32> %first, zeroinitializer
  %second = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %pointers, i32 4, <2 x i1> %second_mask, <2 x i32> zeroinitializer)
  ret <2 x i32> %second
}

; CHECK-LABEL: define <2 x i32> @branch_masks(
; CHECK: left:
; CHECK: %left_mask = icmp sgt
; CHECK: [[LEFT_BASE:%.*]] = call <2 x i64> @__ptsan_load_base({{.*}}<2 x i1> %left_mask)
; CHECK: [[LEFT_RAW:%.*]] = call <2 x ptr> @__ptsan_strip_id32({{.*}}<2 x i64> [[LEFT_BASE]])
; CHECK: call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> [[LEFT_RAW]],
; CHECK: right:
; CHECK: %right_mask = icmp slt
; CHECK: [[RIGHT_BASE:%.*]] = call <2 x i64> @__ptsan_load_base({{.*}}<2 x i1> %right_mask)
; CHECK: [[RIGHT_RAW:%.*]] = call <2 x ptr> @__ptsan_strip_id32({{.*}}<2 x i64> [[RIGHT_BASE]])
; CHECK: call void @llvm.masked.scatter.v2i32.v2p0({{.*}}<2 x ptr> [[RIGHT_RAW]],
define <2 x i32> @branch_masks(<2 x ptr> %pointers, <2 x i64> %indices, i1 %choose) {
  br i1 %choose, label %left, label %right
left:
  %left_mask = icmp sgt <2 x i64> %indices, zeroinitializer
  %value = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %pointers, i32 4, <2 x i1> %left_mask, <2 x i32> zeroinitializer)
  ret <2 x i32> %value
right:
  %right_mask = icmp slt <2 x i64> %indices, zeroinitializer
  call void @llvm.masked.scatter.v2i32.v2p0(<2 x i32> <i32 17, i32 23>, <2 x ptr> %pointers, i32 4, <2 x i1> %right_mask)
  ret <2 x i32> zeroinitializer
}
