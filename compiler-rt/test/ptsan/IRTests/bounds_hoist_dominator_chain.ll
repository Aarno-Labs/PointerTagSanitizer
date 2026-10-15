;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=optimization %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-abort-on-error %s -o /dev/null
;
; Search the dominator chain, but retain the freeing-call barriers on paths
; through sibling blocks and loop backedges.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @may_free(ptr)

define i32 @branch_barrier(ptr %p, i1 %choose) {
entry:
  br i1 %choose, label %freeing, label %join
freeing:
  call void @may_free(ptr %p)
  br label %join
join:
  %pad = add i32 1, 2
  br label %use
use:
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: define i32 @branch_barrier(
; CHECK: entry:
; CHECK-NOT: @__ptsan_load_
; CHECK: call void @may_free
; CHECK-NOT: @__ptsan_load_
; CHECK: join:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: br label %use
; CHECK: use:
; CHECK-NOT: @__ptsan_load_
; CHECK: ret i32

define i32 @loop_barrier(ptr %p, i1 %again) {
entry:
  br label %loop
loop:
  %v = load i32, ptr %p
  br i1 %again, label %freeing, label %exit
freeing:
  call void @may_free(ptr %p)
  br label %loop
exit:
  ret i32 %v
}

; CHECK-LABEL: define i32 @loop_barrier(
; CHECK: entry:
; CHECK-NOT: @__ptsan_load_
; CHECK: loop:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: load i32
; CHECK: call void @may_free

define i32 @unrelated_barrier(ptr %p, i1 %choose) {
entry:
  %pad = add i32 1, 2
  br i1 %choose, label %freeing, label %use
freeing:
  call void @may_free(ptr %p)
  ret i32 0
use:
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: define i32 @unrelated_barrier(
; CHECK: entry:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: br i1
; CHECK: use:
; CHECK-NOT: @__ptsan_load_
; CHECK: ret i32
