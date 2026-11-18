; RUN: %opt_ptsan -verify-each -ptsan-32-bit-ids -ptsan-strip-ptrtoint \
; RUN:   -ptsan-stop-after=late-optimization %s | FileCheck %s
; RUN: %opt_ptsan -verify-each -ptsan-32-bit-ids -ptsan-strip-ptrtoint \
; RUN:   -ptsan-runtime-validate %s -o /dev/null
;
; ID recovery for %a recursively constructs %b's and %c's ID PHIs. When
; constructing %nextc's ID select, %b's ID PHI is complete but %a's is not.
; Explicitly stripping the integer casts requires that recovery. Simplifying
; the resulting select during construction used to inspect the missing incoming
; edge of %a's ID PHI and assert in isSelectWithIdenticalPHI.
;
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i64 @__ptsan_get_id(ptr) nofree

define i64 @recursive_ids(ptr %p, ptr %q, ptr %r, i1 %choose, i1 %done) {
; CHECK-LABEL: define i64 @recursive_ids(
; CHECK-DAG: [[P:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; CHECK-DAG: [[Q:%.*]] = call i64 @__ptsan_get_id(ptr %q)
; CHECK-DAG: [[R:%.*]] = call i64 @__ptsan_get_id(ptr %r)
; CHECK: loop:
; CHECK-DAG: %a.ptsan.id = phi i64 [ %nexta.ptsan.id, %loop ], [ [[P]], %loop.preheader ]
; CHECK-DAG: %b.ptsan.id = phi i64 [ %nextb.ptsan.id, %loop ], [ [[Q]], %loop.preheader ]
; CHECK-DAG: %c.ptsan.id = phi i64 [ %nextc.ptsan.id, %loop ], [ [[R]], %loop.preheader ]
; CHECK: %nexta.ptsan.id = select i1 %choose, i64 %b.ptsan.id, i64 %c.ptsan.id
; CHECK: %nextb.ptsan.id = select i1 %choose, i64 [[P]], i64 %b.ptsan.id
; CHECK: %nextc.ptsan.id = select i1 %choose, i64 %a.ptsan.id, i64 %b.ptsan.id
entry:
  br label %loop
loop:
  %a = phi ptr [ %p, %entry ], [ %nexta, %loop ]
  %b = phi ptr [ %q, %entry ], [ %nextb, %loop ]
  %c = phi ptr [ %r, %entry ], [ %nextc, %loop ]
  %id = call i64 @__ptsan_get_id(ptr %a)
  %aa = ptrtoint ptr %a to i64
  %ab = ptrtoint ptr %b to i64
  %ac = ptrtoint ptr %c to i64
  %v1 = load volatile i8, ptr %a
  %v2 = load volatile i8, ptr %b
  %v3 = load volatile i8, ptr %c
  %nexta = select i1 %choose, ptr %b, ptr %c
  %nextb = select i1 %choose, ptr %p, ptr %b
  %nextc = select i1 %choose, ptr %a, ptr %b
  br i1 %done, label %exit, label %loop
exit:
  %result = xor i64 %id, %aa
  %result2 = xor i64 %result, %ab
  %result3 = xor i64 %result2, %ac
  ret i64 %result3
}
