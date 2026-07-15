; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; Loop preanalysis metadata can contain structured values such as serialized
; GEPs. If preparation cannot synthesize one of those values, optimization's
; later lookup-only pass must treat that range as unavailable rather than
; trying to synthesize new IR after discovery has already run.
;
; This test gives the preanalysis entry a valid base but an unusable range: the
; range min is a serialized GEP whose base is a missing value-id lookup. The pass
; should skip the preanalysis range, keep the normal loop check, and not assert
; in lookup-only deserialization.

; CHECK-NOT: PREANALYZED_LOOP
; CHECK: private unnamed_addr constant [5 x i8] c"LOOP\00"
; CHECK-NOT: PREANALYZED_LOOP
; CHECK-LABEL: define void @lookup_only_structured_value(
; CHECK: call void @ptsan_complain

; ModuleID = 'lookup_only_structured_value.ll'
source_filename = "lookup_only_structured_value.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @lookup_only_structured_value(ptr %buf) {
entry:
  %known.base = alloca i8, align 1, !ptsan.value.id !0
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %gep = getelementptr inbounds i8, ptr %buf, i64 %iv
  store i8 0, ptr %gep, align 1
  %next = add nuw nsw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop, !llvm.loop !1

exit:
  ret void
}

!0 = distinct !{!"ptsan.value.id"}
!1 = distinct !{!1, !2}
!2 = !{!"ptsan.loop.bounds", i1 true, !3}
!3 = !{!"base", !4, !5}
!4 = !{!"lookup", !0}
!5 = !{!"SimpleAccessRange", !6, !7}
!6 = !{!"scev", i16 15, !8}
!7 = !{!"scev", i16 0, i64 4}
!8 = !{!"gep", !9, !10, !11, i64 0}
!9 = distinct !{!"ptsan.value.id.missing"}
!10 = !{!"int", i32 8}
!11 = !{!"lookup", !12}
!12 = distinct !{!"ptsan.value.id.missing.base"}
