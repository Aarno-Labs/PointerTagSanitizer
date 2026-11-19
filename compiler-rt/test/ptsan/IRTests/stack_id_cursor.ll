;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stack-temporal-safety=0 -ptsan-stop-after=lowering %s | FileCheck %s --check-prefixes=CHECK,ID16
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stack-temporal-safety=0 -ptsan-stop-after=lowering %s | FileCheck %s --check-prefixes=CHECK,ID32

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

declare void @escape(ptr) nounwind

; CHECK-DAG: @ptsan_next_stack_id = external thread_local(initialexec) global i64
; CHECK-DAG: @ptsan_stack_id_limit = external thread_local global i64
; CHECK-LABEL: define void @one_id(
; CHECK: [[START:%.*]] = load i64, ptr @ptsan_next_stack_id
; CHECK: [[NEXT:%.*]] = add i64 [[START]], 1
; CHECK: [[LIMIT:%.*]] = load i64, ptr @ptsan_stack_id_limit
; CHECK: %has.stack.id.room = icmp ule i64 [[NEXT]], [[LIMIT]]
; CHECK: br i1 %has.stack.id.room,
; ID16: trunc i64 [[START]] to i16
; ID32: trunc i64 [[START]] to i32
; CHECK: store i64 [[NEXT]], ptr @ptsan_next_stack_id
; CHECK: call void @escape(
; CHECK: store i64 [[START]], ptr @ptsan_next_stack_id
; CHECK: ret void
define void @one_id() {
entry:
  %local = alloca i8
  call void @escape(ptr %local)
  ret void
}
