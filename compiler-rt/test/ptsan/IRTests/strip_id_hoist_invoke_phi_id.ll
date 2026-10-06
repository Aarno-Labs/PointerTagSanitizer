; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s -S | FileCheck %s

; This is the ID-only sibling of strip_id_hoist_invoke_phi.ll. The invoke-result
; PHI itself stays tagged, but a stripped descendant needs a retag at loop exit.
; Computing that retag ID structurally as a PHI of incoming IDs is illegal for
; the invoke arm, because the invoke result's ptrtoint/lshr cannot be inserted
; in the invoke predecessor block.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @may_throw()
declare ptr @__ptsan_strip(ptr)
declare i32 @__gxx_personality_v0(...)

define void @invoke_result_phi_descendant_retag_id(ptr %other, ptr %slot,
                                                   i1 %cond)
    personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @invoke_result_phi_descendant_retag_id(
; CHECK: invoke.pred:
; CHECK-NEXT: %p = invoke ptr @may_throw()
; CHECK: join:
; CHECK-NEXT: %phi = phi ptr [ %p, %invoke.pred ], [ %other, %null.pred ]
; CHECK-NEXT: [[PHI_ASINT:%.*]] = ptrtoint ptr %phi to i64
; CHECK-NEXT: [[PHI_BITS:%.*]] = call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[PHI_ASINT]], i64 281474976710655)
; CHECK-NEXT: [[PHI_STRIP:%.*]] = inttoptr i64 [[PHI_BITS]] to ptr
; CHECK-NEXT: [[PHI_ID:%.*]] = lshr i64 [[PHI_ASINT]], 48
; CHECK: loop:
; CHECK: %g = getelementptr i8, ptr [[PHI_STRIP]], i64 %iv, !ptsan.stripped {{![0-9]+}}
; CHECK: exit:
; CHECK-NEXT: [[G_LCSSA_ASINT:%.*]] = ptrtoint ptr %g to i64
; CHECK-NEXT: [[G_TAGBITS:%.*]] = shl i64 [[PHI_ID]], 48
; CHECK-NEXT: [[G_TAGGED_ASINT:%.*]] = or i64 [[G_LCSSA_ASINT]], [[G_TAGBITS]]
; CHECK-NEXT: [[G_TAGGED:%.*]] = inttoptr i64 [[G_TAGGED_ASINT]] to ptr
; CHECK-NEXT: store ptr [[G_TAGGED]]
entry:
  br i1 %cond, label %invoke.pred, label %null.pred

invoke.pred:
  ; Important: this value can be used directly as a PHI incoming, but derived
  ; ID materialization for that incoming edge has no legal insertion point.
  %p = invoke ptr @may_throw()
          to label %join unwind label %lpad

null.pred:
  br label %join

join:
  ; Important: the PHI remains tagged because one input is an invoke result.
  ; Its stripped descendants can still need a retag ID.
  %phi = phi ptr [ %p, %invoke.pred ], [ %other, %null.pred ]
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %g = getelementptr i8, ptr %phi, i64 %iv
  %s0 = call ptr @__ptsan_strip(ptr %g)
  %v0 = load volatile i8, ptr %s0, align 1
  %g1 = getelementptr i8, ptr %g, i64 1
  %s1 = call ptr @__ptsan_strip(ptr %g1)
  %v1 = load volatile i8, ptr %s1, align 1
  %g2 = getelementptr i8, ptr %g, i64 2
  %s2 = call ptr @__ptsan_strip(ptr %g2)
  %v2 = load volatile i8, ptr %s2, align 1
  %g3 = getelementptr i8, ptr %g, i64 3
  %s3 = call ptr @__ptsan_strip(ptr %g3)
  %v3 = load volatile i8, ptr %s3, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ; Important: this tagged exit of the stripped descendant asks for a retag ID
  ; whose ancestry goes through the invoke-result PHI.
  %g.lcssa = phi ptr [ %g, %loop ]
  store ptr %g.lcssa, ptr %slot
  ret void

lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  resume { ptr, i32 } %lp
}
