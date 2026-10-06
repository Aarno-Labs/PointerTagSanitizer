; RUN: opt -passes=loop-unroll-and-jam -allow-unroll-and-jam \
; RUN:   -unroll-and-jam-count=4 -unroll-remainder -S < %s | FileCheck %s
;
; Unroll-and-jam preserves instruction membership in the jammed body and
; remainder copies, including nested membership on the inner loads. Explicit
; follow-up attributes need not carry PTSan identity on llvm.loop.

target datalayout = "e-m:e-p:32:32-i64:64-v128:64:128-a:0:32-n32-S64"

define void @followup(i32 %outer.count, i32 %inner.count,
                      ptr noalias nocapture %a,
                      ptr noalias nocapture readonly %b) {
entry:
  %has.inner = icmp ne i32 %inner.count, 0
  %has.outer = icmp ne i32 %outer.count, 0
  %has.iterations = and i1 %has.inner, %has.outer
  br i1 %has.iterations, label %outer.preheader, label %exit

outer.preheader:
  br label %outer

outer:
  %i = phi i32 [ %i.next, %outer.latch ], [ 0, %outer.preheader ]
  br label %inner

inner:
  %j = phi i32 [ 0, %outer ], [ %j.next, %inner ]
  %sum = phi i32 [ 0, %outer ], [ %sum.next, %inner ]
  %b.ptr = getelementptr inbounds i32, ptr %b, i32 %j
  %value = load i32, ptr %b.ptr, align 4, !ptsan.loop.ids !11
  %sum.next = add i32 %value, %sum
  %j.next = add nuw i32 %j, 1
  %inner.done = icmp eq i32 %j.next, %inner.count
  br i1 %inner.done, label %outer.latch, label %inner

outer.latch:
  %sum.lcssa = phi i32 [ %sum.next, %inner ]
  %a.ptr = getelementptr inbounds i32, ptr %a, i32 %i
  store i32 %sum.lcssa, ptr %a.ptr, align 4, !ptsan.loop.ids !5
  %i.next = add nuw i32 %i, 1
  %outer.done = icmp eq i32 %i.next, %outer.count
  br i1 %outer.done, label %exit.loopexit, label %outer, !llvm.loop !0

exit.loopexit:
  br label %exit

exit:
  ret void
}

!0 = !{!0, !1, !2, !3, !4, !6}
!1 = !{!"llvm.loop.unroll_and_jam.enable"}
!2 = !{!"llvm.loop.unroll_and_jam.followup_outer", !7}
!3 = !{!"llvm.loop.unroll_and_jam.followup_inner", !8}
!4 = !{!"llvm.loop.unroll_and_jam.followup_all", !9}
!5 = !{i64 42}
!6 = !{!"llvm.loop.unroll_and_jam.followup_remainder_inner", !10}
!7 = !{!"FollowupOuter"}
!8 = !{!"FollowupInner"}
!9 = !{!"FollowupAll"}
!10 = !{!"FollowupRemainderInner"}
!11 = !{i64 42, i64 43}

; CHECK-LABEL: define void @followup(
; CHECK-LABEL: inner:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[INNER_IDS:[0-9]+]]
; CHECK-COUNT-3: load i32, ptr {{.*}}, !ptsan.loop.ids ![[INNER_IDS]]
; CHECK-LABEL: outer.latch:
; CHECK: store i32 {{.*}}, !ptsan.loop.ids ![[OUTER_IDS:[0-9]+]]
; CHECK-COUNT-3: store i32 {{.*}}, !ptsan.loop.ids ![[OUTER_IDS]]
; CHECK-LABEL: inner.epil:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[INNER_IDS]]
; CHECK-LABEL: outer.latch.epil:
; CHECK: store i32 {{.*}}, !ptsan.loop.ids ![[OUTER_IDS]]
; CHECK-LABEL: inner.epil.1:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[INNER_IDS]]
; CHECK-LABEL: outer.latch.epil.1:
; CHECK: store i32 {{.*}}, !ptsan.loop.ids ![[OUTER_IDS]]
; CHECK-LABEL: inner.epil.2:
; CHECK: load i32, ptr {{.*}}, !ptsan.loop.ids ![[INNER_IDS]]
; CHECK-LABEL: outer.latch.epil.2:
; CHECK: store i32 {{.*}}, !ptsan.loop.ids ![[OUTER_IDS]]

; CHECK-DAG: ![[INNER_IDS]] = !{i64 42, i64 43}
; CHECK-DAG: ![[OUTER_IDS]] = !{i64 42}
