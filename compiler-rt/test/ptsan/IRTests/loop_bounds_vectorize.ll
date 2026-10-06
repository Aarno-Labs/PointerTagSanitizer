; RUN: opt -passes=loop-vectorize -force-vector-width=4 \
; RUN:   -force-vector-interleave=1 -S < %s | FileCheck %s
;
; Vectorized and scalar-epilogue accesses retain instruction membership,
; including when explicit follow-up metadata replaces the loop attributes.
; No PTSan identity on llvm.loop is needed.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

define void @followup(ptr nocapture %a, i32 %n) {
entry:
  %has.iterations = icmp sgt i32 %n, 0
  br i1 %has.iterations, label %loop, label %exit

loop:
  %iv = phi i64 [ %iv.next, %loop ], [ 0, %entry ]
  %ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %value = trunc i64 %iv to i32
  store i32 %value, ptr %ptr, align 4, !ptsan.loop.ids !4
  %iv.next = add i64 %iv, 1
  %iv.next.i32 = trunc i64 %iv.next to i32
  %done = icmp eq i32 %iv.next.i32, %n
  br i1 %done, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

define void @default(ptr nocapture %a, i32 %n) {
entry:
  %has.iterations = icmp sgt i32 %n, 0
  br i1 %has.iterations, label %loop, label %exit

loop:
  %iv = phi i64 [ %iv.next, %loop ], [ 0, %entry ]
  %ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %value = trunc i64 %iv to i32
  store i32 %value, ptr %ptr, align 4, !ptsan.loop.ids !4
  %iv.next = add i64 %iv, 1
  %iv.next.i32 = trunc i64 %iv.next to i32
  %done = icmp eq i32 %iv.next.i32, %n
  br i1 %done, label %exit, label %loop, !llvm.loop !8

exit:
  ret void
}

!0 = distinct !{!0, !1, !2, !3}
!1 = !{!"llvm.loop.vectorize.followup_vectorized", !5}
!2 = !{!"llvm.loop.vectorize.followup_epilogue", !6}
!3 = !{!"llvm.loop.vectorize.followup_all", !7}
!4 = !{i64 42}
!5 = !{!"FollowupVectorized"}
!6 = !{!"FollowupEpilogue"}
!7 = !{!"FollowupAll"}
!8 = distinct !{!8}

; CHECK-LABEL: define void @followup(
; CHECK-LABEL: vector.body:
; CHECK: store <4 x i32> {{.*}}, !ptsan.loop.ids ![[IDS:[0-9]+]]
; CHECK-LABEL: loop:
; CHECK: store i32 {{.*}}, !ptsan.loop.ids ![[IDS]]

; CHECK-LABEL: define void @default(
; CHECK-LABEL: vector.body:
; CHECK: store <4 x i32> {{.*}}, !ptsan.loop.ids ![[IDS]]
; CHECK-LABEL: loop:
; CHECK: store i32 {{.*}}, !ptsan.loop.ids ![[IDS]]

; CHECK: ![[IDS]] = !{i64 42}
