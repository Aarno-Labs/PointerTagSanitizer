;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=optimization -ptsan-no-filter-removable-checks %s | FileCheck %s --check-prefix=OPT
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-no-filter-removable-checks %s | FileCheck %s --check-prefix=LOWER

; Origin metadata is a normal check dependency. Two accesses merged into one
; check should retain one origin load alongside their shared base and span, and
; final lowering must consume the anchor.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i8 @two_reads(ptr %p) {
; OPT-LABEL: define i8 @two_reads(
; OPT: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; OPT-DAG: [[BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; OPT-DAG: [[SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[ID]])
; OPT-DAG: [[ORIGIN:%.*]] = call i32 @__ptsan_load_origin(i64 [[ID]])
; OPT-NOT: call i32 @__ptsan_load_origin
; OPT: call void (...) @__ptsan_access(i64 [[ID]], i64 [[BASE]], i64 [[SPAN]], {{.*}}, i32 [[ORIGIN]])
; OPT: load volatile i8
; OPT: load volatile i8
;
; LOWER-LABEL: define i8 @two_reads(
; LOWER-NOT: @__ptsan_load_origin
; LOWER: load volatile i8
entry:
  %a = load volatile i8, ptr %p, align 1
  %b = load volatile i8, ptr %p, align 1
  %result = xor i8 %a, %b
  ret i8 %result
}

define i8 @loop_read(ptr %p, i64 %n) {
; OPT-LABEL: define i8 @loop_read(
; OPT: entry:
; OPT: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; OPT: [[ORIGIN:%.*]] = call i32 @__ptsan_load_origin(i64 [[ID]])
; OPT: br label %loop
; OPT: loop:
; OPT-NOT: call i32 @__ptsan_load_origin
; OPT: load volatile i8
;
; LOWER-LABEL: define i8 @loop_read(
; LOWER: entry:
; LOWER: load ptr, ptr @ptsan_origins
; LOWER: load i32
; LOWER: br {{.*}}label %loop
; LOWER: loop:
; LOWER-NOT: @ptsan_origins
; LOWER: load volatile i8
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %last = phi i8 [ 0, %entry ], [ %value, %loop ]
  %value = load volatile i8, ptr %p, align 1
  %next = add nuw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %last
}
