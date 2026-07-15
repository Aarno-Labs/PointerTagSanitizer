; RUN: opt -passes=loop-distribute -verify-loop-info -verify-dom-info \
; RUN:   -S < %s | FileCheck %s
;
; PTSan loop bounds describe every access in the original loop. They remain a
; conservative description of each partition and of the versioning fallback.

define void @followup(ptr %a, ptr %b, ptr %c, ptr %d, ptr %e) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %a.ptr = getelementptr inbounds i32, ptr %a, i64 %iv
  %a.value = load i32, ptr %a.ptr, align 4
  %b.ptr = getelementptr inbounds i32, ptr %b, i64 %iv
  %b.value = load i32, ptr %b.ptr, align 4
  %ab = mul i32 %a.value, %b.value
  %iv.next = add nuw nsw i64 %iv, 1
  %a.next.ptr = getelementptr inbounds i32, ptr %a, i64 %iv.next
  store i32 %ab, ptr %a.next.ptr, align 4
  %d.ptr = getelementptr inbounds i32, ptr %d, i64 %iv
  %d.value = load i32, ptr %d.ptr, align 4
  %e.ptr = getelementptr inbounds i32, ptr %e, i64 %iv
  %e.value = load i32, ptr %e.ptr, align 4
  %de = mul i32 %d.value, %e.value
  %c.ptr = getelementptr inbounds i32, ptr %c, i64 %iv
  store i32 %de, ptr %c.ptr, align 4
  %done = icmp eq i64 %iv.next, 20
  br i1 %done, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1, !2, !3, !4, !5, !6}
!1 = !{!"llvm.loop.distribute.enable", i1 true}
!2 = !{!"llvm.loop.distribute.followup_all", !7}
!3 = !{!"llvm.loop.distribute.followup_coincident", !8}
!4 = !{!"llvm.loop.distribute.followup_sequential", !9}
!5 = !{!"llvm.loop.distribute.followup_fallback", !10}
!6 = !{!"ptsan.loop.bounds"}
!7 = !{!"FollowupAll"}
!8 = !{!"FollowupCoincident", i1 false}
!9 = !{!"FollowupSequential", i32 8}
!10 = !{!"FollowupFallback"}

; CHECK-LABEL: define void @followup(
; CHECK: br i1 {{.*}}, label {{.*}}, label %loop.lver.orig, !llvm.loop ![[FALLBACK:[0-9]+]]
; CHECK: br i1 {{.*}}, label {{.*}}, label %loop.ldist1, !llvm.loop ![[SEQUENTIAL:[0-9]+]]
; CHECK: br i1 {{.*}}, label {{.*}}, label %loop, !llvm.loop ![[COINCIDENT:[0-9]+]]

; CHECK: ![[FALLBACK]] = distinct !{![[FALLBACK]], ![[BOUNDS:[0-9]+]], ![[ALL:[0-9]+]], ![[FALLBACK_ATTR:[0-9]+]]}
; CHECK: ![[BOUNDS]] = !{!"ptsan.loop.bounds"}
; CHECK: ![[ALL]] = !{!"FollowupAll"}
; CHECK: ![[FALLBACK_ATTR]] = !{!"FollowupFallback"}
; CHECK: ![[SEQUENTIAL]] = distinct !{![[SEQUENTIAL]], ![[ALL]], ![[SEQUENTIAL_ATTR:[0-9]+]], ![[BOUNDS]]}
; CHECK: ![[SEQUENTIAL_ATTR]] = !{!"FollowupSequential", i32 8}
; CHECK: ![[COINCIDENT]] = distinct !{![[COINCIDENT]], ![[ALL]], ![[COINCIDENT_ATTR:[0-9]+]], ![[BOUNDS]]}
; CHECK: ![[COINCIDENT_ATTR]] = !{!"FollowupCoincident", i1 false}
