;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DISCOVERY
; RUN: %opt_ptsan -ptsan-stop-after=lowering %s | FileCheck %s --check-prefixes=LOWER,TEMPORAL
; RUN: %opt_ptsan -ptsan-stack-temporal-safety=0 -ptsan-stop-after=lowering %s | FileCheck %s --check-prefixes=LOWER,IMMEDIATE
; RUN: %opt_ptsan %s -o /dev/null

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @setjmp(ptr) nounwind returns_twice
declare void @consume(ptr) nounwind

; Even a destination without any frame IDs must reclaim skipped callees' IDs.
define i32 @no_sources(ptr %env) nounwind {
entry:
  %result = call i32 @setjmp(ptr %env)
  ret i32 %result
}

; DISCOVERY-LABEL: define i32 @no_sources(
; DISCOVERY: call void (...) @__ptsan_setjmp()
; DISCOVERY-NEXT: %result = call i32 @setjmp(

; LOWER-LABEL: define i32 @no_sources(
; LOWER: [[CHECKPOINT:%.*]] = alloca [2 x i64]
; LOWER: call void @ptsan_save_stack_id_checkpoint(ptr [[CHECKPOINT]])
; LOWER: [[NEXT:%.*]] = load i16, ptr @ptsan_next_stack_id
; TEMPORAL: [[PENDING:%.*]] = load i16, ptr @ptsan_pending_stack_ids
; LOWER: %result = call i32 @setjmp(
; LOWER-NEXT: [[AGAIN:%.*]] = icmp ne i32 %result, 0
; LOWER-NEXT: br i1 [[AGAIN]], label %[[RESTORE:[^,]+]], label %[[CONTINUE:.*]]
; LOWER: [[RESTORE]]:
; LOWER: call void @ptsan_restore_stack_id_checkpoint(ptr [[CHECKPOINT]])
; TEMPORAL: load i16, ptr @ptsan_next_stack_id
; TEMPORAL: call void @llvm.memset
; TEMPORAL: store i16 [[PENDING]], ptr @ptsan_pending_stack_ids
; IMMEDIATE-NOT: @llvm.memset
; LOWER: store i16 [[NEXT]], ptr @ptsan_next_stack_id
; LOWER: br label %[[CONTINUE]]
; LOWER: [[CONTINUE]]:
; LOWER: ret i32 %result

; The snapshot must follow allocation of the destination's own fixed IDs and
; repeatable-alloca container, not run anew on every iteration of setjmp.
define void @own_sources(ptr %env, i64 %size) nounwind {
entry:
  %fixed = alloca [32 x i8]
  call void @consume(ptr %fixed)
  br label %loop
loop:
  %dynamic = alloca i8, i64 %size
  call void @consume(ptr %dynamic)
  %result = call i32 @setjmp(ptr %env)
  %again = icmp eq i32 %result, 0
  br i1 %again, label %loop, label %exit
exit:
  call void @consume(ptr %fixed)
  ret void
}

; LOWER-LABEL: define void @own_sources(
; LOWER: call ptr @ptsan_get_repeatable_alloca_ids_container()
; LOWER: call i16 @ptsan_get_stack_ids_1(
; LOWER: call void @ptsan_save_stack_id_checkpoint(
; LOWER: call void @consume(
; LOWER: loop:
; LOWER-NOT: call void @ptsan_save_stack_id_checkpoint
; LOWER: call i16 @ptsan_get_repeatable_alloca_id(
; LOWER: %result = call i32 @setjmp(
; LOWER: call void @ptsan_restore_stack_id_checkpoint(
