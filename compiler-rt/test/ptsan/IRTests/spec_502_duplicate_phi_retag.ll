; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s -S -o /dev/null

; 502.gcc_r hits a PHI with duplicate incoming edges from the same predecessor.
; When min-cut decides the incoming value should be stripped but the PHI should
; be tagged, PTSan retags each duplicate incoming use independently. Lowering
; used to turn those into two distinct values from the same block. Preparation
; now canonicalizes duplicate PHI entries before PTSan reasons about them.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)

define void @duplicate_phi_predecessor_retag(ptr %p, ptr %slot0, ptr %slot1,
                                             ptr %slot2) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %latch ]
  %g = getelementptr i8, ptr %p, i64 %iv
  %s0 = call ptr @__ptsan_strip(ptr %g) ; stripped loop use
  %v0 = load volatile i8, ptr %s0, align 1
  %g1 = getelementptr i8, ptr %g, i64 1
  %s1 = call ptr @__ptsan_strip(ptr %g1) ; extra strip saving under %g
  %v1 = load volatile i8, ptr %s1, align 1
  switch i8 %v0, label %latch [
    i8 10, label %exit
    i8 13, label %exit
  ]

latch:
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 8
  br i1 %done, label %exit, label %loop

exit:
  ; The first two entries intentionally have the same predecessor and the same
  ; original value. LLVM allows that only while the incoming values remain
  ; identical.
  %dup = phi ptr [ %g, %loop ], [ %g, %loop ], [ %p, %latch ]
  ; Multiple tagged exits make retagging the incoming PHI edges cheaper than
  ; retagging each store separately.
  store ptr %dup, ptr %slot0
  store ptr %dup, ptr %slot1
  store ptr %dup, ptr %slot2
  ret void
}
