;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DISCOVERY
; RUN: %opt_ptsan -ptsan-stop-after=lowering %s | FileCheck %s --check-prefix=LOWER
; RUN: %opt_ptsan -ptsan-stack-temporal-safety=0 -ptsan-stop-after=lowering %s | FileCheck %s --check-prefix=LOWER
; RUN: %opt_ptsan %s -o /dev/null

; ID32 uses the same cursor cleanup, with full-width ID arguments.
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DISCOVERY
; RUN: %opt_ptsan -ptsan-32-bit-ids %s -o /dev/null
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stack-temporal-safety=0 -ptsan-stop-after=lowering %s | FileCheck %s --check-prefix=LOWER

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
; LOWER: [[CHECKPOINT:%.*]] = load i64, ptr @ptsan_stack_id_log
; LOWER: [[NEXT:%.*]] = load i64, ptr @ptsan_next_stack_id
; LOWER: [[PENDING:%.*]] = load i{{16|32}}, ptr @ptsan_pending_stack_ids
; LOWER: [[SIGNAL:%.*]] = load i8, ptr @ptsan_in_signal_context
; LOWER-NOT: load i32, ptr @ptsan_in_libc
; LOWER: %result = call i32 @setjmp(
; LOWER-NEXT: [[AGAIN:%.*]] = icmp ne i32 %result, 0
; LOWER-NEXT: br i1 [[AGAIN]], label %[[RESTORE:[^,]+]], label %[[CONTINUE:.*]]
; LOWER: [[RESTORE]]:
; LOWER-NEXT: call void @ptsan_restore_stack_ids(i64 [[CHECKPOINT]],
; LOWER-SAME: i64 [[NEXT]], i{{16|32}} [[PENDING]], i8 [[SIGNAL]])
; LOWER-NEXT: br label %[[CONTINUE]]
; LOWER: [[CONTINUE]]:
; LOWER: ret i32 %result

; Each loop iteration saves a fresh snapshot after its dynamic allocation,
; immediately before setjmp. Function exit instead uses the frame-entry cursor.
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
; LOWER: br i1 %has.stack.id.room, label %[[FAST:[^,]+]], label %[[SLOW:[^,]+]],
; LOWER: [[FAST]]:
; LOWER: store i64 {{.*}}, ptr @ptsan_next_stack_id
; LOWER-NEXT: [[FASTCURSOR:%.*]] = load i64, ptr @ptsan_stack_id_log
; LOWER-NEXT: br label %[[MERGE:.*]]
; LOWER: [[SLOW]]:
; LOWER-NEXT: [[SLOWCURSOR:%.*]] = load i64, ptr @ptsan_stack_id_log
; LOWER-NEXT: call void @ptsan_get_stack_ids(
; LOWER: [[FRAME:%.*]] = phi i64 [ [[FASTCURSOR]], %[[FAST]] ], [ [[SLOWCURSOR]], %[[SLOW]] ]
; LOWER: call void @consume(
; LOWER: loop:
; LOWER: call i{{16|32}} @ptsan_get_repeatable_alloca_id(
; LOWER: call void @consume(
; LOWER: [[JUMP:%.*]] = load i64, ptr @ptsan_stack_id_log
; LOWER: %result = call i32 @setjmp(
; LOWER: call void @ptsan_restore_stack_ids(i64 [[JUMP]],
; LOWER: call void @ptsan_free_stack_ids(i64 [[FRAME]])

; Fixed-only fast allocation must not load the log cursor or call the runtime.
define void @fixed_only() nounwind {
entry:
  %fixed = alloca [32 x i8]
  call void @consume(ptr %fixed)
  ret void
}

; LOWER-LABEL: define void @fixed_only(
; LOWER-NOT: @ptsan_stack_id_log
; LOWER: br i1 %has.stack.id.room, label %[[FIXEDFAST:[^,]+]], label %[[FIXEDSLOW:[^,]+]],
; LOWER: [[FIXEDFAST]]:
; LOWER: store i64 {{.*}}, ptr @ptsan_next_stack_id
; LOWER-NEXT: br label %[[FIXEDMERGE:.*]]
; LOWER: [[FIXEDSLOW]]:
; LOWER-NEXT: [[FIXEDCURSOR:%.*]] = load i64, ptr @ptsan_stack_id_log
; LOWER-NEXT: call void @ptsan_get_stack_ids(
; LOWER: phi i64 [ 0, %[[FIXEDFAST]] ], [ [[FIXEDCURSOR]], %[[FIXEDSLOW]] ]

; With no fixed IDs, save at entry without a fixed-ID allocation call.
define void @repeatable_only(i64 %size, i1 %again) nounwind {
entry:
  br label %loop
loop:
  %dynamic = alloca i8, i64 %size
  call void @consume(ptr %dynamic)
  br i1 %again, label %loop, label %exit
exit:
  ret void
}

; LOWER-LABEL: define void @repeatable_only(
; LOWER: entry:
; LOWER-NEXT: [[REPEATCURSOR:%.*]] = load i64, ptr @ptsan_stack_id_log
; LOWER-NOT: @ptsan_get_stack_ids
; LOWER: loop:
; LOWER: call i{{16|32}} @ptsan_get_repeatable_alloca_id(
; LOWER: call void @ptsan_free_stack_ids(i64 [[REPEATCURSOR]])
; LOWER: ret void

; LOWER: declare void @ptsan_restore_stack_ids(i64, i64, i{{16 zeroext|32}}, i8 zeroext)
