; RUN: opt -passes=loop-unroll -unroll-runtime=true \
; RUN:   -unroll-runtime-epilog=true -S < %s | FileCheck %s
; RUN: opt -passes=loop-unroll -unroll-runtime=true \
; RUN:   -unroll-runtime-epilog=false -S < %s | FileCheck %s \
; RUN:   --check-prefix=PROLOG
;
; PTSan loop-preanalysis bounds are semantic information, not an unroll hint.
; Preserve exactly one copy on both the unrolled and remainder loops even when
; explicit follow-up metadata replaces the unroller's automatic attributes.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

define i32 @followup(ptr nocapture %a, i32 %n) nounwind readonly {
entry:
  %empty = icmp eq i32 %n, 0
  br i1 %empty, label %exit, label %loop

loop:
  %iv = phi i64 [ %iv.next, %loop ], [ 0, %entry ]
  %sum = phi i32 [ %sum.next, %loop ], [ 0, %entry ]
  %ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %value = load i32, ptr %ptr, align 4
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
  %value = load i32, ptr %ptr, align 4
  %sum.next = add i32 %value, %sum
  %iv.next = add i64 %iv, 1
  %iv.next.i32 = trunc i64 %iv.next to i32
  %done = icmp eq i32 %iv.next.i32, %n
  br i1 %done, label %exit, label %loop, !llvm.loop !8

exit:
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result
}

!0 = distinct !{!0, !1, !2, !3, !4}
!1 = !{!"llvm.loop.unroll.followup_all", !5}
!2 = !{!"llvm.loop.unroll.followup_unrolled", !6}
!3 = !{!"llvm.loop.unroll.followup_remainder", !7}
!4 = !{!"ptsan.loop.bounds"}
!5 = !{!"FollowupAll"}
!6 = !{!"FollowupUnrolled"}
!7 = !{!"FollowupRemainder"}
!8 = distinct !{!8, !4}

; CHECK-LABEL: define i32 @followup(
; CHECK: br i1 {{.*}}, label {{.*}}, label %loop, !llvm.loop ![[UNROLLED:[0-9]+]]
; CHECK: br i1 {{.*}}, label %loop.epil, label {{.*}}, !llvm.loop ![[REMAINDER:[0-9]+]]

; CHECK-LABEL: define i32 @default(
; CHECK: br i1 {{.*}}, label {{.*}}, label %loop, !llvm.loop ![[DEFAULT_UNROLLED:[0-9]+]]
; CHECK: br i1 {{.*}}, label %loop.epil, label {{.*}}, !llvm.loop ![[DEFAULT_REMAINDER:[0-9]+]]

; CHECK: ![[UNROLLED]] = distinct !{![[UNROLLED]], ![[ALL:[0-9]+]], ![[UNROLLED_ATTR:[0-9]+]], ![[BOUNDS:[0-9]+]]}
; CHECK: ![[ALL]] = !{!"FollowupAll"}
; CHECK: ![[UNROLLED_ATTR]] = !{!"FollowupUnrolled"}
; CHECK: ![[BOUNDS]] = !{!"ptsan.loop.bounds"}
; CHECK: ![[REMAINDER]] = distinct !{![[REMAINDER]], ![[ALL]], ![[REMAINDER_ATTR:[0-9]+]], ![[BOUNDS]]}
; CHECK: ![[REMAINDER_ATTR]] = !{!"FollowupRemainder"}
; CHECK: ![[DEFAULT_UNROLLED]] = distinct !{![[DEFAULT_UNROLLED]], ![[BOUNDS]]}
; CHECK: ![[DEFAULT_REMAINDER]] = distinct !{![[DEFAULT_REMAINDER]], ![[UNROLL_DISABLE:[0-9]+]], ![[BOUNDS]]}
; CHECK: ![[UNROLL_DISABLE]] = !{!"llvm.loop.unroll.disable"}

; PROLOG-LABEL: define i32 @default(
; PROLOG-LABEL: loop.prol:
; PROLOG: br i1 {{.*}}, label %loop.prol, label {{.*}}, !llvm.loop ![[PROLOG_REMAINDER:[0-9]+]]
; PROLOG-LABEL: loop:
; PROLOG: br i1 {{.*}}, label {{.*}}, label %loop, !llvm.loop ![[PROLOG_UNROLLED:[0-9]+]]

; PROLOG: ![[PROLOG_BOUNDS:[0-9]+]] = !{!"ptsan.loop.bounds"}
; PROLOG: ![[PROLOG_REMAINDER]] = distinct !{![[PROLOG_REMAINDER]], ![[PROLOG_DISABLE:[0-9]+]], ![[PROLOG_BOUNDS]]}
; PROLOG: ![[PROLOG_DISABLE]] = !{!"llvm.loop.unroll.disable"}
; PROLOG: ![[PROLOG_UNROLLED]] = distinct !{![[PROLOG_UNROLLED]], ![[PROLOG_BOUNDS]]}
