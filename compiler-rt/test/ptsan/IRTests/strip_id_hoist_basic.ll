; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Basic profitable case: a loop repeatedly strips a GEP derived from %b, and
; there are no tagged sibling users of %b. The pass should strip %b once before
; the loop and feed the loop GEP from that stripped base.
define void @basic_loop_gep(ptr %b) {
; CHECK-LABEL: define void @basic_loop_gep(
; CHECK: entry:
; CHECK: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; CHECK: loop:
; CHECK: %g = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: load volatile i8, ptr %g
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr inbounds i8, ptr %b, i64 %iv ; neutral derived pointer
  %v = load volatile i8, ptr %g, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; If two neutral uses reconverge on the same strip anchor, that strip should
; only be counted once. This straight-line case has one removable strip and one
; new base strip, so hoisting would be a wash; double-counting the select's two
; operands would make it incorrectly look profitable.
define void @duplicate_select_use_not_profitable(ptr %b, i1 %c) {
; CHECK-LABEL: define void @duplicate_select_use_not_profitable(
; CHECK-NOT: call ptr @__ptsan_strip(ptr %b
; CHECK: %g = getelementptr inbounds i8, ptr %b, i64 8
; CHECK: %sel = select i1 %c, ptr %g, ptr %g
; CHECK: [[STRIP:%.*]] = call ptr @__ptsan_strip(ptr %sel
; CHECK: load volatile i8, ptr [[STRIP]]
entry:
  %g = getelementptr inbounds i8, ptr %b, i64 8 ; duplicate neutral input
  %sel = select i1 %c, ptr %g, ptr %g ; both arms reach the same strip
  %v = load volatile i8, ptr %sel, align 1
  ret void
}

; Freeze preserves the concrete bits of its operand, so it should participate
; in ordinary pointer dataflow rather than forcing its operand to stay tagged.
; The two stripped consumers can therefore share one conversion across it.
define void @freeze_is_transparent(ptr %b) {
; CHECK-LABEL: define void @freeze_is_transparent(
; CHECK: %g = getelementptr inbounds i8, ptr %b, i64 8
; CHECK: [[STRIP:%.*]] = call ptr @__ptsan_strip(ptr %g
; CHECK: load volatile i8, ptr [[STRIP]]
; CHECK: %f = freeze ptr [[STRIP]]
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: load volatile i8, ptr %f
entry:
  %g = getelementptr inbounds i8, ptr %b, i64 8
  %before = load volatile i8, ptr %g
  %f = freeze ptr %g
  %after = load volatile i8, ptr %f
  ret void
}
