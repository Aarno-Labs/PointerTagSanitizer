;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @sink(ptr)

; The loop PHI and select both derive entirely from %a, so their conversions
; should reuse %a's ID instead of constructing recurrent ID PHIs/selects.
define void @loop_select(ptr %a, i1 %choose) {
; CHECK-LABEL: define void @loop_select(
; CHECK: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-NEXT: [[ENTRY_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[A_ID]])
; CHECK-NEXT: call ptr @__ptsan_strip_id32(ptr %a, i64 [[A_ID]], i64 [[ENTRY_BASE]])
; CHECK: %cursor = phi ptr
; CHECK-NOT: .ptsan.id = phi
; CHECK-NOT: call i64 @__ptsan_load_base(i64 [[A_ID]])
; CHECK: call ptr @__ptsan_tag_id32(ptr %early, i64 [[A_ID]], i64 [[ENTRY_BASE]])
; CHECK: call void @sink(
; CHECK: %sel = select i1 %choose, ptr %cursor, ptr %initial
; CHECK-NOT: .ptsan.id = select
; CHECK-NOT: call i64 @__ptsan_load_base(i64 [[A_ID]])
; CHECK-NEXT: call ptr @__ptsan_tag_id32(ptr %sel, i64 [[A_ID]], i64 [[ENTRY_BASE]])
entry:
  %initial = getelementptr i8, ptr %a, i64 0
  br label %loop

loop:
  %cursor = phi ptr [ %initial, %entry ], [ %next, %loop ]
  %early = getelementptr i8, ptr %cursor, i64 4
  call void @sink(ptr %early)
  %x = getelementptr i8, ptr %cursor, i64 8
  %vx = load volatile i8, ptr %x
  %y = getelementptr i8, ptr %cursor, i64 16
  %vy = load volatile i8, ptr %y
  %z = getelementptr i8, ptr %cursor, i64 24
  %vz = load volatile i8, ptr %z
  %sel = select i1 %choose, ptr %cursor, ptr %initial
  call void @sink(ptr %sel)
  %sx = getelementptr i8, ptr %sel, i64 8
  %svx = load volatile i8, ptr %sx
  %sy = getelementptr i8, ptr %sel, i64 16
  %svy = load volatile i8, ptr %sy
  %sz = getelementptr i8, ptr %sel, i64 24
  %svz = load volatile i8, ptr %sz
  %next = getelementptr i8, ptr %sel, i64 1
  br label %loop
}
