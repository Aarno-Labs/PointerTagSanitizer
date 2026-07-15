; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; 500.perlbench_r exposed a dominance bug when stripped PHIs fed other PHIs.
; The global solver may choose to keep the inner PHI tagged and strip only the
; loop-internal use, avoiding any synthesized ID PHI for the outer mixed PHI.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)

define void @phi_feeds_phi_without_id_phi(ptr %base, ptr %other, ptr %slot,
                                          i1 %choose.inner,
                                          i1 %choose.outer) {
; CHECK-LABEL: define void @phi_feeds_phi_without_id_phi(
; CHECK-NOT: .ptsan.id = phi
; CHECK: inner.join:
; CHECK: %inner = phi ptr [ %left.gep, %left ], [ %right.gep, %right ]
; CHECK-NEXT: [[INNER_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %inner)
; CHECK: outer.join:
; CHECK: %outer = phi ptr [ %inner, %outer.left ], [ %other, %outer.right ]
; CHECK-NEXT: [[OUTER_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %outer)
; CHECK-NEXT: store ptr [[OUTER_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %use = getelementptr i8, ptr [[INNER_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %use
entry:
  br i1 %choose.inner, label %left, label %right

left:
  %left.gep = getelementptr i8, ptr %base, i64 8
  br label %inner.join

right:
  %right.gep = getelementptr i8, ptr %base, i64 16
  br label %inner.join

inner.join:
  %inner = phi ptr [ %left.gep, %left ], [ %right.gep, %right ]
  br i1 %choose.outer, label %outer.left, label %outer.right

outer.left:
  br label %outer.join

outer.right:
  br label %outer.join

outer.join:
  %outer = phi ptr [ %inner, %outer.left ], [ %other, %outer.right ]
  store ptr %outer, ptr %slot
  br label %loop

loop:
  %iv = phi i64 [ 0, %outer.join ], [ %next, %loop ]
  %use = getelementptr i8, ptr %inner, i64 %iv
  %use.s = call ptr @__ptsan_strip(ptr %use)
  %v = load volatile i8, ptr %use.s, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
