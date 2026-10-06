;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks \
; RUN:   -ptsan-stop-after=lowering %s | FileCheck %s

; GetID returns one i64 per pointer lane. MinCut generates a Tag for the raw
; select below, with a matching ID vector, and lowering installs each lane's ID.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i64 @__ptsan_get_id(ptr)
declare <4 x ptr> @__ptsan_strip(<4 x ptr>)

define <4 x i64> @lower_vector_get_id(<4 x ptr> %value) {
; CHECK-LABEL: define <4 x i64> @lower_vector_get_id(
; CHECK-NOT: extractelement
; CHECK: [[BITS:%.*]] = ptrtoint <4 x ptr> %value to <4 x i64>
; CHECK-NEXT: [[IDS:%.*]] = lshr <4 x i64> [[BITS]], <i64 48, i64 48, i64 48, i64 48>
; CHECK-NEXT: ret <4 x i64> [[IDS]]
entry:
  %ids = call <4 x i64> @__ptsan_get_id(<4 x ptr> %value)
  ret <4 x i64> %ids
}

define <4 x ptr> @lower_vector_strip(<4 x ptr> %value) {
; CHECK-LABEL: define <4 x ptr> @lower_vector_strip(
; CHECK: [[RAW:%.*]] = call <4 x ptr> @llvm.ptrmask.v4p0.v4i64(
; CHECK-SAME: <4 x ptr> %value,
; CHECK-SAME: <4 x i64> <i64 281474976710655, i64 281474976710655, i64 281474976710655, i64 281474976710655>)
; CHECK-NEXT: ret <4 x ptr> [[RAW]]
entry:
  %raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> %value)
  ret <4 x ptr> %raw
}

define <4 x ptr> @lower_vector_tag(<4 x ptr> %left, <4 x ptr> %right,
                                 <4 x i1> %condition) {
; CHECK-LABEL: define <4 x ptr> @lower_vector_tag(
; CHECK: [[IDS:%.*]] = select <4 x i1> %condition, <4 x i64> {{%.*}}, <4 x i64> {{%.*}}
; CHECK: [[VALUE_BITS:%.*]] = ptrtoint <4 x ptr> %value to <4 x i64>
; CHECK-NEXT: [[TAG_BITS:%.*]] = shl <4 x i64> [[IDS]], <i64 48, i64 48, i64 48, i64 48>
; CHECK-NEXT: [[TAGGED_BITS:%.*]] = or <4 x i64> [[VALUE_BITS]], [[TAG_BITS]]
; CHECK-NEXT: [[TAGGED:%.*]] = inttoptr <4 x i64> [[TAGGED_BITS]] to <4 x ptr>
; CHECK-NEXT: ret <4 x ptr> [[TAGGED]]
entry:
  %left.raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> %left)
  %right.raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> %right)
  %value = select <4 x i1> %condition, <4 x ptr> %left.raw,
                                     <4 x ptr> %right.raw
  ret <4 x ptr> %value
}

; All compiler-only anchors must be gone after lowering.
; CHECK-NOT: call {{.*}} @__ptsan_
