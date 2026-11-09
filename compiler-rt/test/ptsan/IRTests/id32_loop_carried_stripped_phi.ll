;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks \
; RUN:   -ptsan-stop-after=late-optimization %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip_id32(ptr, i64, i64) nofree
declare i64 @__ptsan_get_id(ptr) nofree
declare i64 @__ptsan_load_base(i64) nofree

; Hoisting strips to %base makes the whole pointer recurrence stripped. The
; incoming strip fixes the cycle's polarity even though the PHI and backedge
; GEP otherwise depend on each other.
define void @loop_carried_stripped_phi(ptr %base, i32 %count) {
; CHECK-LABEL: define void @loop_carried_stripped_phi(
; CHECK: entry:
; CHECK: [[BASE:%.*]] = call ptr @__ptsan_strip_id32(ptr %base{{.*}}), !ptsan.stripped
; CHECK: loop:
; CHECK: %cursor = phi ptr {{.*}}[[BASE]]{{.*}}, !ptsan.stripped {{![0-9]+}}
; CHECK: %field = getelementptr i8, ptr %cursor, i64 4, !ptsan.stripped {{![0-9]+}}
; CHECK: %next = getelementptr i8, ptr %cursor, i64 8, !ptsan.stripped {{![0-9]+}}
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %cursor = phi ptr [ %base, %entry ], [ %next, %loop ]
  %field = getelementptr i8, ptr %cursor, i64 4
  %field.id = call i64 @__ptsan_get_id(ptr %cursor)
  %field.base = call i64 @__ptsan_load_base(i64 %field.id)
  %field.strip = call ptr @__ptsan_strip_id32(ptr %field, i64 %field.id, i64 %field.base)
  %value = load volatile i8, ptr %field.strip, align 1
  %next = getelementptr i8, ptr %cursor, i64 8
  %next.id = call i64 @__ptsan_get_id(ptr %cursor)
  %next.base = call i64 @__ptsan_load_base(i64 %next.id)
  %next.strip = call ptr @__ptsan_strip_id32(ptr %next, i64 %next.id, i64 %next.base)
  %next.value = load volatile i8, ptr %next.strip, align 1
  %inc = add nuw i32 %i, 1
  %done = icmp eq i32 %inc, %count
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
