; RUN: opt -passes=loop-vectorize -force-vector-width=4 \
; RUN:   -force-vector-interleave=1 -S < %s | FileCheck %s
;
; PTSan loop-preanalysis bounds are semantic information, not a vectorizer
; hint. Preserve exactly one copy on both loops even when explicit follow-up
; metadata replaces the vectorizer's automatically generated loop attributes.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

define void @followup(ptr nocapture %a, i32 %n) {
entry:
  %has.iterations = icmp sgt i32 %n, 0
  br i1 %has.iterations, label %loop, label %exit

loop:
  %iv = phi i64 [ %iv.next, %loop ], [ 0, %entry ]
  %ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %value = trunc i64 %iv to i32
  store i32 %value, ptr %ptr, align 4
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
  store i32 %value, ptr %ptr, align 4
  %iv.next = add i64 %iv, 1
  %iv.next.i32 = trunc i64 %iv.next to i32
  %done = icmp eq i32 %iv.next.i32, %n
  br i1 %done, label %exit, label %loop, !llvm.loop !8

exit:
  ret void
}

!0 = distinct !{!0, !1, !2, !3, !4}
!1 = !{!"llvm.loop.vectorize.followup_vectorized", !5}
!2 = !{!"llvm.loop.vectorize.followup_epilogue", !6}
!3 = !{!"llvm.loop.vectorize.followup_all", !7}
!4 = !{!"ptsan.loop.bounds"}
!5 = !{!"FollowupVectorized"}
!6 = !{!"FollowupEpilogue"}
!7 = !{!"FollowupAll"}
!8 = distinct !{!8, !4}

; CHECK-LABEL: define void @followup(
; CHECK-LABEL: vector.body:
; CHECK: br i1 {{.*}}, label %middle.block, label %vector.body, !llvm.loop ![[VECTOR:[0-9]+]]
; CHECK-LABEL: loop:
; CHECK: br i1 %done, label %exit.loopexit, label %loop, !llvm.loop ![[EPILOGUE:[0-9]+]]

; CHECK-LABEL: define void @default(
; CHECK-LABEL: vector.body:
; CHECK: br i1 {{.*}}, label %middle.block, label %vector.body, !llvm.loop ![[DEFAULT_VECTOR:[0-9]+]]
; CHECK-LABEL: loop:
; CHECK: br i1 %done, label %exit.loopexit, label %loop, !llvm.loop ![[DEFAULT_EPILOGUE:[0-9]+]]

; CHECK: ![[VECTOR]] = distinct !{![[VECTOR]], ![[ALL:[0-9]+]], ![[VECTORIZED:[0-9]+]], ![[UNROLL_DISABLE:[0-9]+]], ![[BOUNDS:[0-9]+]]}
; CHECK: ![[ALL]] = !{!"FollowupAll"}
; CHECK: ![[VECTORIZED]] = !{!"FollowupVectorized"}
; CHECK: ![[UNROLL_DISABLE]] = !{!"llvm.loop.unroll.runtime.disable"}
; CHECK: ![[BOUNDS]] = !{!"ptsan.loop.bounds"}
; CHECK: ![[EPILOGUE]] = distinct !{![[EPILOGUE]], ![[ALL]], ![[EPILOGUE_ATTR:[0-9]+]], ![[BOUNDS]]}
; CHECK: ![[EPILOGUE_ATTR]] = !{!"FollowupEpilogue"}
; CHECK: ![[DEFAULT_VECTOR]] = distinct !{![[DEFAULT_VECTOR]], ![[BOUNDS]], ![[IS_VECTORIZED:[0-9]+]], ![[UNROLL_DISABLE]]}
; CHECK: ![[IS_VECTORIZED]] = !{!"llvm.loop.isvectorized", i32 1}
; CHECK: ![[DEFAULT_EPILOGUE]] = distinct !{![[DEFAULT_EPILOGUE]], ![[BOUNDS]], ![[UNROLL_DISABLE]], ![[IS_VECTORIZED]]}
