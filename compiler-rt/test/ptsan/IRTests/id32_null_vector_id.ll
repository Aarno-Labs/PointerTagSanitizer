;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-reoptimize=0 \
; RUN:   -ptsan-stop-after=late-optimization %s | FileCheck %s

; A zero pointer vector has a scalar-common ID. Its explicit ID request and
; conversion base load should remain scalar rather than loading per lane.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <4 x ptr> @__ptsan_strip(<4 x ptr>, i64, i64) nofree
declare i64 @__ptsan_get_id(ptr) nofree
declare i64 @__ptsan_load_base(i64) nofree

define void @strip_zero_vector() {
; CHECK-LABEL: define void @strip_zero_vector(
; CHECK: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr null)
; CHECK-NEXT: [[BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; CHECK-NEXT: call <4 x ptr> @__ptsan_strip(<4 x ptr> zeroinitializer, i64 [[ID]], i64 [[BASE]])
entry:
  %id = call i64 @__ptsan_get_id(ptr null)
  %base = call i64 @__ptsan_load_base(i64 %id)
  %raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> zeroinitializer, i64 %id, i64 %base)
  call void asm sideeffect "", "v"(<4 x ptr> %raw)
  ret void
}
