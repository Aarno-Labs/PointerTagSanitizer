; RUN: %opt_ptsan -ptsan-reoptimize=0 -ptsan-clear-ids-on-arith=0 -ptsan-clear-ids-on-cmp=0 -ptsan-preanalyze-loops=0 %s -S | FileCheck %s --check-prefix=OPT
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-reoptimize=0 -ptsan-clear-ids-on-arith=0 -ptsan-clear-ids-on-cmp=0 -ptsan-preanalyze-loops=0 %s -S | FileCheck %s --check-prefix=NOOPTS

; PTSan no-opts used to choose the landingpad store itself as the check
; insertion point, materialize check operands before the invoke, then assert
; because the collapsed common insertion point named the invoke.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; OPT-LABEL: define void @store_to_alloca_from_landingpad(
; NOOPTS-LABEL: define void @store_to_alloca_from_landingpad(
define void @store_to_alloca_from_landingpad() personality ptr null {
entry:
  %slot = alloca ptr, align 8
  invoke void null()
          to label %return unwind label %lpad

lpad:
  %lp = landingpad { ptr, i32 }
          catch ptr null
  ; OPT: landingpad
  ; OPT: store ptr null, ptr
  ; NOOPTS: landingpad
  ; NOOPTS: store ptr null, ptr
  store ptr null, ptr %slot, align 8
  ret void

return:
  ret void
}
