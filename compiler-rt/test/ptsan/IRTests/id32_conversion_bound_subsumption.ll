;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=late-optimization %s | FileCheck %s

; A base load used by a check must remain above @ret_arg, which may free the
; object. The later base load is used only to decode %r, so it can reuse the
; dominating load even though the call separates them.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @ret_arg(ptr returned)
declare ptr @__ptsan_strip_id32(ptr, i64, i64) nofree
declare i64 @__ptsan_get_id(ptr) nofree
declare i64 @__ptsan_load_base(i64) nofree

define i64 @conversion_reuses_dominating_check_base(ptr %p) {
; CHECK-LABEL: define i64 @conversion_reuses_dominating_check_base(
; CHECK: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; CHECK: [[BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; CHECK: [[ORIGIN:%.*]] = call i32 @__ptsan_load_origin(i64 [[ID]])
; CHECK: load volatile i8
; CHECK: %r = call ptr @ret_arg(
; CHECK-NOT: call i64 @__ptsan_load_base
; CHECK: [[STRIPPED:%.*]] = call ptr @__ptsan_strip_id32(ptr %r, i64 [[ID]], i64 [[BASE]])
; CHECK: ptrtoint ptr [[STRIPPED]] to i64
entry:
  %value = load volatile i8, ptr %p, align 1
  %r = call ptr @ret_arg(ptr %p)
  %strip.id = call i64 @__ptsan_get_id(ptr %p)
  %strip.base = call i64 @__ptsan_load_base(i64 %strip.id)
  %stripped = call ptr @__ptsan_strip_id32(ptr %r, i64 %strip.id, i64 %strip.base)
  %bits = ptrtoint ptr %stripped to i64
  %value.ext = zext i8 %value to i64
  %result = xor i64 %bits, %value.ext
  ret i64 %result
}

define i8 @check_keeps_post_call_base(ptr %p) {
; CHECK-LABEL: define i8 @check_keeps_post_call_base(
; CHECK: [[KEEP_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; CHECK: [[BEFORE_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[KEEP_ID]])
; CHECK-DAG: call ptr @__ptsan_strip_id32(ptr %p, i64 [[KEEP_ID]], i64 [[BEFORE_BASE]])
; CHECK-DAG: [[BEFORE_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[KEEP_ID]])
; CHECK-DAG: [[BEFORE_ORIGIN:%.*]] = call i32 @__ptsan_load_origin(i64 [[KEEP_ID]])
; CHECK: call void (...) @__ptsan_access(i64 [[KEEP_ID]], i64 [[BEFORE_BASE]], i64 [[BEFORE_SPAN]], {{.*}}, i32 [[BEFORE_ORIGIN]])
; CHECK: load volatile i8
; CHECK: %r = call ptr @ret_arg(
; CHECK: [[AFTER_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[KEEP_ID]])
; CHECK: [[AFTER_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[KEEP_ID]])
; CHECK: [[AFTER_ORIGIN:%.*]] = call i32 @__ptsan_load_origin(i64 [[KEEP_ID]])
; The decode can reuse the earlier bias, but the check must use fresh bounds.
; CHECK: call void (...) @__ptsan_access(i64 [[KEEP_ID]], i64 [[AFTER_BASE]], i64 [[AFTER_SPAN]], {{.*}}, i32 [[AFTER_ORIGIN]])
; CHECK: call ptr @__ptsan_strip_id32(ptr %r, i64 [[KEEP_ID]], i64 [[BEFORE_BASE]])
; CHECK: load volatile i8
entry:
  %before = load volatile i8, ptr %p, align 1
  %r = call ptr @ret_arg(ptr %p)
  %after = load volatile i8, ptr %r, align 1
  %result = xor i8 %before, %after
  ret i8 %result
}
