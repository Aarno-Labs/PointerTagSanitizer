;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; Preparation folds exact byte-address cancellation, retaining the saved offset
; for other users. A different base, truncated address, or scaled GEP must not
; be mistaken for that cancellation. No PTSan anchors are supplied as input.
; RUN: %opt_ptsan -ptsan-stop-after=prepare %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=prepare %s | FileCheck %s

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

define ptr @cancel(ptr %p, ptr %saved) {
; CHECK-LABEL: define ptr @cancel(
; CHECK: store i64 %negative, ptr %saved
; CHECK: ret ptr null
  %bits = ptrtoint ptr %p to i64
  %negative = sub i64 0, %bits
  store i64 %negative, ptr %saved
  %result = getelementptr inbounds i8, ptr %p, i64 %negative
  ret ptr %result
}

define ptr @different_base(ptr %p, ptr %q) {
; CHECK-LABEL: define ptr @different_base(
; CHECK: getelementptr i8, ptr %q, i64 %negative
; CHECK: ret ptr %result
  %bits = ptrtoint ptr %p to i64
  %negative = sub i64 0, %bits
  %result = getelementptr i8, ptr %q, i64 %negative
  ret ptr %result
}

; A non-null target can belong to a different allocation. Recover that pointer,
; not a pointer carrying the reconstruction base's ID, and retain offset users.
define ptr @recover_target(ptr %base, ptr %target, ptr %saved) {
; CHECK-LABEL: define ptr @recover_target(
; CHECK: store i64 %offset, ptr %saved
; CHECK: ret ptr %target
  %base.bits = ptrtoint ptr %base to i64
  %target.bits = ptrtoint ptr %target to i64
  %offset = sub i64 %target.bits, %base.bits
  store i64 %offset, ptr %saved
  %result = getelementptr i8, ptr %base, i64 %offset
  ret ptr %result
}

define ptr @different_nonnull_base(ptr %base, ptr %target, ptr %other) {
; CHECK-LABEL: define ptr @different_nonnull_base(
; CHECK: getelementptr i8, ptr %other, i64 %offset
; CHECK: ret ptr %result
  %base.bits = ptrtoint ptr %base to i64
  %target.bits = ptrtoint ptr %target to i64
  %offset = sub i64 %target.bits, %base.bits
  %result = getelementptr i8, ptr %other, i64 %offset
  ret ptr %result
}

define ptr @truncated(ptr %p) {
; CHECK-LABEL: define ptr @truncated(
; CHECK: getelementptr i8, ptr %p, i32 %negative
; CHECK: ret ptr %result
  %bits = ptrtoint ptr %p to i32
  %negative = sub i32 0, %bits
  %result = getelementptr i8, ptr %p, i32 %negative
  ret ptr %result
}

define ptr @scaled(ptr %p) {
; CHECK-LABEL: define ptr @scaled(
; CHECK: getelementptr i64, ptr %p, i64 %negative
; CHECK: ret ptr %result
  %bits = ptrtoint ptr %p to i64
  %negative = sub i64 0, %bits
  %result = getelementptr i64, ptr %p, i64 %negative
  ret ptr %result
}
