;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: split-file %s %t
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks \
; RUN:   -ptsan-stop-after=lowering %t/id16.ll | FileCheck %t/id16.ll
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-no-opts -ptsan-nochecks \
; RUN:   -ptsan-stop-after=lowering %t/id32.ll | FileCheck %t/id32.ll

; Exercise GetID, Strip, and Tag lowering directly with encoding-specific anchors.
; These tests intentionally construct anchors to check their emitted operations.

;--- id16.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <4 x i64> @__ptsan_get_id(<4 x ptr>) nofree
declare <4 x ptr> @__ptsan_strip(<4 x ptr>) nofree

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
; CHECK-NOT: call {{.*}} @__ptsan_
entry:
  %left.raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> %left)
  %right.raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> %right)
  %value = select <4 x i1> %condition, <4 x ptr> %left.raw,
                                     <4 x ptr> %right.raw
  ret <4 x ptr> %value
}
;--- id32.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <4 x i64> @__ptsan_get_id(<4 x ptr>) nofree
declare <4 x ptr> @__ptsan_strip_id32(<4 x ptr>, <4 x i64>, <4 x i64>) nofree
declare <4 x i64> @__ptsan_load_base(<4 x i64>, <4 x i1>) nofree

define <4 x i64> @lower_vector_get_id(<4 x ptr> %value) {
; CHECK-LABEL: define <4 x i64> @lower_vector_get_id(
; CHECK-NOT: extractelement
; CHECK: [[BITS:%.*]] = ptrtoint <4 x ptr> %value to <4 x i64>
; CHECK-NEXT: [[HIGH:%.*]] = lshr <4 x i64> [[BITS]], <i64 32, i64 32, i64 32, i64 32>
; CHECK-NEXT: [[ENRICHED:%.*]] = icmp slt <4 x i64> [[BITS]], <i64 -1, i64 -1, i64 -1, i64 -1>
; CHECK-NEXT: [[IDS:%.*]] = select <4 x i1> [[ENRICHED]], <4 x i64> [[HIGH]], <4 x i64> zeroinitializer
; CHECK-NEXT: ret <4 x i64> [[IDS]]
entry:
  %ids = call <4 x i64> @__ptsan_get_id(<4 x ptr> %value)
  ret <4 x i64> %ids
}

define <4 x ptr> @lower_vector_strip(<4 x ptr> %value) {
; CHECK-LABEL: define <4 x ptr> @lower_vector_strip(
; CHECK: [[BITS:%.*]] = ptrtoint <4 x ptr> %value to <4 x i64>
; CHECK: [[BASES:%.*]] = call <4 x i64> @llvm.masked.gather
; CHECK: [[PAYLOAD:%.*]] = trunc <4 x i64> [[BITS]] to <4 x i32>
; CHECK: [[WIDE:%.*]] = zext <4 x i32> [[PAYLOAD]] to <4 x i64>
; CHECK: [[DECODED:%.*]] = add <4 x i64> [[BASES]], [[WIDE]]
; CHECK: [[RAW_BITS:%.*]] = select <4 x i1> {{%.*}}, <4 x i64> [[DECODED]], <4 x i64> [[BITS]]
; CHECK: [[RAW:%.*]] = inttoptr <4 x i64> [[RAW_BITS]] to <4 x ptr>
; CHECK-NEXT: ret <4 x ptr> [[RAW]]
entry:
  %raw.id = call <4 x i64> @__ptsan_get_id(<4 x ptr> %value)
  %raw.base = call <4 x i64> @__ptsan_load_base(<4 x i64> %raw.id, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  %raw = call <4 x ptr> @__ptsan_strip_id32(<4 x ptr> %value, <4 x i64> %raw.id, <4 x i64> %raw.base)
  ret <4 x ptr> %raw
}

define <4 x ptr> @lower_vector_tag(<4 x ptr> %left, <4 x ptr> %right,
                                 <4 x i1> %condition) {
; CHECK-LABEL: define <4 x ptr> @lower_vector_tag(
; CHECK: [[IDS:%.*]] = select <4 x i1> %condition, <4 x i64> {{%.*}}, <4 x i64> {{%.*}}
; CHECK: [[BASES:%.*]] = call <4 x i64> @llvm.masked.gather
; CHECK: [[VALUE_BITS:%.*]] = ptrtoint <4 x ptr> %value to <4 x i64>
; CHECK-NEXT: [[PAYLOAD:%.*]] = sub <4 x i64> [[VALUE_BITS]], [[BASES]]
; CHECK-NEXT: [[HIGH:%.*]] = shl <4 x i64> [[IDS]], <i64 32, i64 32, i64 32, i64 32>
; CHECK-NEXT: [[FITS:%.*]] = icmp ule <4 x i64> [[PAYLOAD]], <i64 4294967295, i64 4294967295, i64 4294967295, i64 4294967295>
; CHECK-NEXT: [[PACKED:%.*]] = or <4 x i64> [[HIGH]], [[PAYLOAD]]
; CHECK-NEXT: [[SAFE:%.*]] = select <4 x i1> [[FITS]], <4 x i64> [[PACKED]], <4 x i64> [[VALUE_BITS]]
; CHECK-NEXT: [[ZERO:%.*]] = icmp eq <4 x i64> [[IDS]], zeroinitializer
; CHECK-NEXT: [[TAGGED_BITS:%.*]] = select <4 x i1> [[ZERO]], <4 x i64> [[VALUE_BITS]], <4 x i64> [[SAFE]]
; CHECK-NEXT: [[TAGGED:%.*]] = inttoptr <4 x i64> [[TAGGED_BITS]] to <4 x ptr>
; CHECK-NEXT: ret <4 x ptr> [[TAGGED]]
; CHECK-NOT: call {{.*}} @__ptsan_
entry:
  %left.raw.id = call <4 x i64> @__ptsan_get_id(<4 x ptr> %left)
  %left.raw.base = call <4 x i64> @__ptsan_load_base(<4 x i64> %left.raw.id, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  %left.raw = call <4 x ptr> @__ptsan_strip_id32(<4 x ptr> %left, <4 x i64> %left.raw.id, <4 x i64> %left.raw.base)
  %right.raw.id = call <4 x i64> @__ptsan_get_id(<4 x ptr> %right)
  %right.raw.base = call <4 x i64> @__ptsan_load_base(<4 x i64> %right.raw.id, <4 x i1> <i1 true, i1 true, i1 true, i1 true>)
  %right.raw = call <4 x ptr> @__ptsan_strip_id32(<4 x ptr> %right, <4 x i64> %right.raw.id, <4 x i64> %right.raw.base)
  %value = select <4 x i1> %condition, <4 x ptr> %left.raw,
                                     <4 x ptr> %right.raw
  ret <4 x ptr> %value
}
