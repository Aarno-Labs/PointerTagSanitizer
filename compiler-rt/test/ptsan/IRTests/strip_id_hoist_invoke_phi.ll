; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s -S | FileCheck %s

; This models a Bullet build failure where an invoke result feeds a PHI in the
; invoke's normal destination. PTSan cannot insert strip/tag/ID materialization
; on that invoke-result edge without splitting it, so the PHI must stay tagged.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @may_throw()
declare ptr @__ptsan_strip(ptr)
declare i32 @__gxx_personality_v0(...)

define void @invoke_result_phi_needs_legal_retag_id(ptr %other, ptr %slot,
                                                    i1 %cond)
    personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @invoke_result_phi_needs_legal_retag_id(
; CHECK: invoke.pred:
; CHECK-NEXT: %p = invoke ptr @may_throw()
; CHECK: join:
; CHECK-NEXT: %phi = phi ptr [ %p, %invoke.pred ], [ %other, %null.pred ], !ptsan.tagged {{![0-9]+}}
; CHECK-NEXT: [[PHI_INT:%.*]] = ptrtoint ptr %phi to i64
; CHECK-NEXT: [[PHI_BITS:%.*]] = call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[PHI_INT]], i64 281474976710655)
; CHECK-NEXT: [[PHI_TRUE:%.*]] = inttoptr i64 [[PHI_BITS]] to ptr
; CHECK-NEXT: store ptr %phi, ptr {{%.*}}
; CHECK: loop:
; CHECK: %g0 = getelementptr i8, ptr [[PHI_TRUE]], i64 %iv, !ptsan.stripped {{![0-9]+}}
; CHECK-NEXT: load volatile i8, ptr %g0
; CHECK: %g1 = getelementptr i8, ptr [[PHI_TRUE]], i64 1, !ptsan.stripped {{![0-9]+}}
; CHECK-NEXT: load volatile i8, ptr %g1
entry:
  br i1 %cond, label %invoke.pred, label %null.pred

invoke.pred:
  ; Important: invoke results are only available on the normal edge. If this
  ; value is used as a PHI incoming, any derived ID/tag computation must not be
  ; inserted in the invoke predecessor block.
  %p = invoke ptr @may_throw()
          to label %join unwind label %lpad

null.pred:
  br label %join

join:
  ; Important: this PHI can become stripped because the loop below has multiple
  ; stripped uses, but the store also requires a tagged form and therefore an ID
  ; for each incoming value.
  %phi = phi ptr [ %p, %invoke.pred ], [ %other, %null.pred ]
  store ptr %phi, ptr %slot
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %g0 = getelementptr i8, ptr %phi, i64 %iv
  %s0 = call ptr @__ptsan_strip(ptr %g0)
  %v0 = load volatile i8, ptr %s0, align 1
  %g1 = getelementptr i8, ptr %phi, i64 1
  %s1 = call ptr @__ptsan_strip(ptr %g1)
  %v1 = load volatile i8, ptr %s1, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void

lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  resume { ptr, i32 } %lp
}
