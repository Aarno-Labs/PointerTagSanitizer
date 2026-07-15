; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; This models the IR produced from:
;
;   void ambiguous_lookup_id(char *buf) {
;     for (unsigned i = 0; i < 4; ++i)
;       buf[i] = 0;
;   }
;
; with preanalysis metadata that looks up a value by !ptsan.value.id.
; The two allocas deliberately share the same value ID, so the lookup is
; ambiguous and must not be used as a known preanalyzed loop bound.

; CHECK-NOT: PREANALYZED_LOOP
; CHECK: private unnamed_addr constant [5 x i8] c"LOOP\00"
; CHECK-NOT: PREANALYZED_LOOP
; CHECK-LABEL: define void @ambiguous_lookup_id(
; CHECK: ptrtoint ptr %buf to i64
; CHECK: call void @ptsan_complain

; ModuleID = 'lookup_ambiguous_id.ll'
source_filename = "lookup_ambiguous_id.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @ambiguous_lookup_id(ptr %buf) {
entry:
  %first = alloca i8, align 1, !ptsan.value.id !0
  %second = alloca i8, align 1, !ptsan.value.id !0
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
!6 = !{!"scev", i16 15, !4}
!7 = !{!"scev", i16 0, i64 4}
