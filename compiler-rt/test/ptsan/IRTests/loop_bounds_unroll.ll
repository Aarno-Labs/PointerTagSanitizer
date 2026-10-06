; RUN: opt -passes=loop-unroll -unroll-runtime=true \
; RUN:   -unroll-runtime-epilog=true -S < %s | FileCheck %s
; RUN: opt -passes=loop-unroll -unroll-runtime=true \
; RUN:   -unroll-runtime-epilog=false -S < %s | FileCheck %s \
; RUN:   --check-prefix=PROLOG
;
; Instruction membership survives unrolling and either remainder placement,
; even when explicit follow-up metadata replaces the loop attributes.
; No PTSan identity on llvm.loop is needed.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

define i32 @followup(ptr nocapture %a, i32 %n) nounwind readonly {
entry:
  %empty = icmp eq i32 %n, 0
  br i1 %empty, label %exit, label %loop

loop:
  %iv = phi i64 [ %iv.next, %loop ], [ 0, %entry ]
  %sum = phi i32 [ %sum.next, %loop ], [ 0, %entry ]
  %ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %value = load i32, ptr %ptr, align 4, !ptsan.loop.ids !4
  %sum.next = add i32 %value, %sum
  %iv.next = add i64 %iv, 1
  %iv.next.i32 = trunc i64 %iv.next to i32
  %done = icmp eq i32 %iv.next.i32, %n
  br i1 %done, label %exit, label %loop, !llvm.loop !0

exit:
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result
}

define i32 @default(ptr nocapture %a, i32 %n) nounwind readonly {
entry:
  %empty = icmp eq i32 %n, 0
  br i1 %empty, label %exit, label %loop

loop:
  %iv = phi i64 [ %iv.next, %loop ], [ 0, %entry ]
  %sum = phi i32 [ %sum.next, %loop ], [ 0, %entry ]
  %ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %value = load i32, ptr %ptr, align 4, !ptsan.loop.ids !4
  %sum.next = add i32 %value, %sum
  %iv.next = add i64 %iv, 1
  %iv.next.i32 = trunc i64 %iv.next to i32
  %done = icmp eq i32 %iv.next.i32, %n
  br i1 %done, label %exit, label %loop, !llvm.loop !8

exit:
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result
}

!0 = distinct !{!0, !1, !2, !3}
!1 = !{!"llvm.loop.unroll.followup_all", !5}
!2 = !{!"llvm.loop.unroll.followup_unrolled", !6}
!3 = !{!"llvm.loop.unroll.followup_remainder", !7}
!4 = !{i64 42}
!5 = !{!"FollowupAll"}
!6 = !{!"FollowupUnrolled"}
!7 = !{!"FollowupRemainder"}
!8 = distinct !{!8}

; CHECK-LABEL: define i32 @followup(
; CHECK-LABEL: loop:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS:[0-9]+]]
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]
; CHECK-LABEL: loop.epil:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]

; CHECK-LABEL: define i32 @default(
; CHECK-LABEL: loop:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]
; CHECK-LABEL: loop.epil:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]

; CHECK: ![[IDS]] = !{i64 42}

; PROLOG-LABEL: define i32 @default(
; PROLOG-LABEL: loop.prol:
; PROLOG: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS:[0-9]+]]
; PROLOG-LABEL: loop:
; PROLOG: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]
; PROLOG: load i32, ptr {{.*}}, !ptsan.loop.ids ![[IDS]]

; PROLOG: ![[IDS]] = !{i64 42}
