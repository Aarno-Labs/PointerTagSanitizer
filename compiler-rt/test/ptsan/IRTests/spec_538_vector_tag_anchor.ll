; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks -ptsan-stop-after=lowering %s -S -o /dev/null

; 538.imagick_r reaches final lowering with a vector-of-pointers tag anchor.
; Tag lowering should keep this in vector IR by splatting the scalar tag bits.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <2 x ptr> @__ptsan_tag(<2 x ptr>, i64)

define <2 x ptr> @vector_tag_anchor_crashes(<2 x ptr> %vec, i64 %id) {
entry:
  ; This models the anchor shape that crashes PTSanLowerer::lowerTag.
  %tagged = call <2 x ptr> @__ptsan_tag(<2 x ptr> %vec, i64 %id)
  ret <2 x ptr> %tagged
}
