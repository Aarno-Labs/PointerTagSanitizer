;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-nochecks %s | FileCheck %s --check-prefix=LOWER
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s
;
; These are allocation/conversion-cost tests. Runtime ID preservation and
; access checking are covered by source_pointer_ids.c and argv_source_ids.c.
; Do not require any particular lifetime or placement of source markers.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

declare void @consume(ptr)
declare void @consume_id(i64)
declare i64 @__ptsan_get_id(ptr)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture)

; Observing this pointer's ID would introduce demand, so use a generated-code
; assertion to check that private storage and lifetime markers cost no IDs.
define i8 @stripped_alloca() {
entry:
  %buffer = alloca [4 x i8], align 1
  call void @llvm.lifetime.start.p0(i64 4, ptr %buffer)
  store volatile i8 7, ptr %buffer
  %value = load volatile i8, ptr %buffer, align 1
  call void @llvm.lifetime.end.p0(i64 4, ptr %buffer)
  ret i8 %value
}

; LOWER-LABEL: define i8 @stripped_alloca(
; LOWER-NOT: ptsan_get_stack_ids
; LOWER: ret i8

; Paying for one conversion outside the loop is sufficient.
define void @tagged_in_loop(i1 %done) {
entry:
  %buffer = alloca [4 x i8], align 1
  br label %loop
loop:
  call void @consume(ptr %buffer)
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @tagged_in_loop(
; CHECK: call i64 @__ptsan_get_id(
; CHECK: call ptr @__ptsan_tag(
; CHECK: loop:
; CHECK-NOT: call i64 @__ptsan_get_id(
; CHECK-NOT: call ptr @__ptsan_tag(
; CHECK: ret void

; ID-only demand must not construct a tagged pointer just to extract its ID.
; Repeated requests must share one allocation.
define void @repeated_ids() {
entry:
  %buffer = alloca [4 x i8], align 1
  %first = call i64 @__ptsan_get_id(ptr %buffer)
  %second = call i64 @__ptsan_get_id(ptr %buffer)
  call void @consume_id(i64 %first)
  call void @consume_id(i64 %second)
  ret void
}

; LOWER-LABEL: define void @repeated_ids(
; LOWER-NOT: lshr
; LOWER: call i16 @ptsan_get_stack_ids_1(ptr nonnull %buffer, i64 4)
; LOWER-NOT: call i16 @ptsan_get_stack_ids
; LOWER-NOT: inttoptr
; LOWER-NOT: lshr
; LOWER: invoke void @consume_id(i64 [[ID:%.*]])
; LOWER: invoke void @consume_id(i64 [[ID]])
