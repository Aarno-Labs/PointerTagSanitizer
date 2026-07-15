; Duplicate strip anchors must be canonicalized before access analysis
; snapshots unstripped operands and SCEVs. Otherwise strip merging can replace
; the strip used by the access range snapshot and leave placement with no valid
; check insertion point.
;
; RUN: %opt_ptsan %s -o /dev/null

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @strip_anchor_snapshot() {
entry:
  %0 = call i64 @__ptsan_strip_i64(i64 0)
  %idx = trunc i64 %0 to i32
  %idx64 = sext i32 %idx to i64
  %ptr = getelementptr i32, ptr null, i64 %idx64
  %1 = load i32, ptr %ptr, align 4
  %2 = call i64 @__ptsan_strip_i64(i64 0)
  ret void
}

declare i64 @__ptsan_strip_i64(i64)
