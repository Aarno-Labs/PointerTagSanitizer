; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; This captures a strip-ID hoisting bug. If a value feeds both a contained PHI
; and an uncontained PHI, we must not create a mixed-polarity contained PHI.
; The global min-cut hoister can keep the mixed exit tagged while using a
; separate stripped representative for the contained PHI's loop subtree.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)

define void @retagged_arm_poisoning_contained_phi(ptr %a, ptr %other,
                                                  ptr %slot, i1 %choose.good,
                                                  i1 %choose.bad) {
; CHECK-LABEL: define void @retagged_arm_poisoning_contained_phi(
; CHECK: entry:
; CHECK-DAG: [[SLOT_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %slot)
; CHECK-DAG: [[A_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %a)
; CHECK: %b = getelementptr i8, ptr %a, i64 8
; CHECK-NEXT: %c = getelementptr i8, ptr %a, i64 16
; CHECK: good.join:
; CHECK: %good = phi ptr [ %b, %good.left ], [ %c, %good.right ]
; CHECK-NEXT: [[GOOD_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %good)
; CHECK: bad.left:
; CHECK: bad.join:
; CHECK: %bad = phi ptr [ %b, %bad.left ], [ %other, %bad.right ]
; CHECK: [[BAD_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %bad)
; CHECK: store ptr [[BAD_TAGGED]], ptr [[SLOT_STRIP]]
; CHECK: loop:
; CHECK: %direct = getelementptr i8, ptr [[A_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %direct
; CHECK: %under.good = getelementptr i8, ptr [[GOOD_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %under.good
entry:
  %b = getelementptr i8, ptr %a, i64 8 ; shared arm
  %c = getelementptr i8, ptr %a, i64 16 ; other contained arm
  br i1 %choose.good, label %good.left, label %good.right

good.left:
  br label %good.join

good.right:
  br label %good.join

good.join:
  %good = phi ptr [ %b, %good.left ], [ %c, %good.right ] ; contained in %a
  br i1 %choose.bad, label %bad.left, label %bad.right

bad.left:
  br label %bad.join

bad.right:
  br label %bad.join

bad.join:
  %bad = phi ptr [ %b, %bad.left ], [ %other, %bad.right ] ; forces %b tagged
  store ptr %bad, ptr %slot ; tagged consumer of the uncontained PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %bad.join ], [ %next, %loop ]
  %direct = getelementptr i8, ptr %a, i64 %iv ; makes the %a hoist profitable
  %direct.s = call ptr @__ptsan_strip(ptr %direct)
  %direct.v = load volatile i8, ptr %direct.s, align 1
  %under.good = getelementptr i8, ptr %good, i64 %iv ; should stay stripped
  %under.good.s = call ptr @__ptsan_strip(ptr %under.good)
  %under.good.v = load volatile i8, ptr %under.good.s, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
