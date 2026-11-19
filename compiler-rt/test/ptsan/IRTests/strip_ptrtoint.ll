;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; Opting into strip-ptrtoint must decode before scalar and vector integer
; conversions, regardless of integer width. Without the flag, keep the bits.

; RUN: %opt_ptsan -ptsan-stop-after=discovery -ptsan-strip-ptrtoint %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DEFAULT
; RUN: %opt_ptsan -ptsan-stop-after=discovery -ptsan-strip-ptrtoint-function=full -ptsan-strip-ptrtoint-function=vector %s | FileCheck %s --check-prefix=SELECTED
; RUN: %opt_ptsan -ptsan-stop-after=discovery -ptsan-strip-ptrtoint -ptsan-strip-ptrtoint-function=full %s | FileCheck %s
; DEFAULT-NOT: call {{.*}}@__ptsan_strip

; Function selection is repeatable and leaves unselected conversions alone.
; SELECTED-LABEL: define i64 @full(
; SELECTED: call ptr @__ptsan_strip(ptr %p
; SELECTED-LABEL: define i32 @narrow(
; SELECTED-NOT: call {{.*}}@__ptsan_strip
; SELECTED-LABEL: define i128 @wide(
; SELECTED-NOT: call {{.*}}@__ptsan_strip
; SELECTED-LABEL: define <2 x i64> @vector(
; SELECTED: call <2 x ptr> @__ptsan_strip

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

define i64 @full(ptr %p) {
; CHECK-LABEL: define i64 @full(
; CHECK: [[RAW:%.*]] = call ptr @__ptsan_strip(ptr %p
; CHECK: ptrtoint ptr [[RAW]] to i64
  %bits = ptrtoint ptr %p to i64
  ret i64 %bits
}

define i32 @narrow(ptr %p) {
; CHECK-LABEL: define i32 @narrow(
; CHECK: [[RAW:%.*]] = call ptr @__ptsan_strip(ptr %p
; CHECK: ptrtoint ptr [[RAW]] to i32
  %bits = ptrtoint ptr %p to i32
  ret i32 %bits
}

define i128 @wide(ptr %p) {
; CHECK-LABEL: define i128 @wide(
; CHECK: [[RAW:%.*]] = call ptr @__ptsan_strip(ptr %p
; CHECK: ptrtoint ptr [[RAW]] to i128
  %bits = ptrtoint ptr %p to i128
  ret i128 %bits
}

define <2 x i64> @vector(<2 x ptr> %p) {
; CHECK-LABEL: define <2 x i64> @vector(
; CHECK: [[RAW:%.*]] = call <2 x ptr> @__ptsan_strip{{.*}}(<2 x ptr> %p
; CHECK: ptrtoint <2 x ptr> [[RAW]] to <2 x i64>
  %bits = ptrtoint <2 x ptr> %p to <2 x i64>
  ret <2 x i64> %bits
}
