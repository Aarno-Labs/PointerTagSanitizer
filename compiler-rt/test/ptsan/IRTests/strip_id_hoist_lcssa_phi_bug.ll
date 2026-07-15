; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; This captures a strip-ID hoisting bug seen in ClamAV. Hoisting to a loop-local
; GEP strips an LCSSA PHI, then a later PHI joins that stripped value with the
; original tagged base. The LCSSA-to-PHI incoming use must be retagged, while
; uses that stay inside the stripped subtree should remain stripped.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)
declare void @escape(ptr)
declare void @use_i64(i64)

define void @contained_lcssa_phi_feeds_mixed_phi(ptr %base.slot, ptr %slot,
                                                 i1 %take.lcssa,
                                                 i1 %store.lcssa) {
; CHECK-LABEL: define void @contained_lcssa_phi_feeds_mixed_phi(
; CHECK: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %base)
; CHECK: [[BASE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %base)
; CHECK: loop:
; CHECK: %idx = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NEXT: %idx.v = load volatile i8, ptr %idx
; CHECK: %idx.1 = getelementptr i8, ptr %idx, i64 1
; CHECK-NOT: call ptr @__ptsan_strip(ptr %idx.1)
; CHECK: %idx.2 = getelementptr i8, ptr %idx, i64 2
; CHECK-NOT: call ptr @__ptsan_strip(ptr %idx.2)
; CHECK: lcssa.block:
; CHECK-NEXT: [[IDX_ID:%.*]] = phi i64 [ [[BASE_ID]], %loop ]
; CHECK-NEXT: %idx.lcssa = phi ptr [ %idx, %loop ]
; CHECK-NEXT: [[IDX_TAG:%.*]] = call ptr @__ptsan_tag(ptr %idx.lcssa, i64 [[IDX_ID]])
; CHECK: store.block:
; CHECK-NEXT: [[STORE_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr [[IDX_TAG]])
; CHECK-NEXT: store ptr [[STORE_TAGGED]]
; CHECK: join:
; CHECK-NEXT: %pt = phi ptr [ %idx.lcssa, %store.block ], [ [[BASE_STRIP]], %lcssa.block ], [ {{%.*}}, %join.loopexit ]
; CHECK-NEXT: %pt.v = load volatile i8, ptr %pt
entry:
  %base = load ptr, ptr %base.slot ; instruction-valued tagged base
  ; Keep hoisting to the original argument unattractive; the interesting
  ; candidate is the loop-local %idx, like ClamAV's %arrayidx.
  call void @escape(ptr %base)
  call void @escape(ptr %base)
  call void @escape(ptr %base)
  call void @escape(ptr %base)
  %base.int = ptrtoint ptr %base to i64 ; keep %base itself from hoisting
  call void @use_i64(i64 %base.int)
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %latch ]
  %idx = getelementptr i8, ptr %base, i64 %iv ; contained candidate subtree
  %idx.s = call ptr @__ptsan_strip(ptr %idx) ; loop strip to hoist
  %idx.v = load volatile i8, ptr %idx.s, align 1
  %idx.1 = getelementptr i8, ptr %idx, i64 1
  %idx.1.s = call ptr @__ptsan_strip(ptr %idx.1) ; extra saving under %idx
  %idx.1.v = load volatile i8, ptr %idx.1.s, align 1
  %idx.2 = getelementptr i8, ptr %idx, i64 2
  %idx.2.s = call ptr @__ptsan_strip(ptr %idx.2) ; extra saving under %idx
  %idx.2.v = load volatile i8, ptr %idx.2.s, align 1
  br i1 %take.lcssa, label %lcssa.block, label %latch

lcssa.block:
  %idx.lcssa = phi ptr [ %idx, %loop ] ; contained in %base
  br i1 %store.lcssa, label %store.block, label %join

store.block:
  store ptr %idx.lcssa, ptr %slot ; ordinary tagged consumer
  br label %join

latch:
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %join, label %loop

join:
  %pt = phi ptr [ %idx.lcssa, %store.block ],
                [ %base, %lcssa.block ],
                [ %base, %latch ] ; contained in %base, but mixed polarity
  %pt.s = call ptr @__ptsan_strip(ptr %pt)
  %pt.v = load volatile i8, ptr %pt.s, align 1
  ret void
}
