; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; An existing strip of %p can also serve stripped users introduced by global
; hoisting. The optimizer should count and emit that conversion once rather than
; keeping a second strip below the GEP.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)

define i16 @existing_strip_is_shared_conversion(ptr %p) {
; CHECK-LABEL: define i16 @existing_strip_is_shared_conversion(
; CHECK: entry:
; CHECK-NEXT: [[P_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; CHECK-NEXT: %direct = load volatile i8, ptr [[P_STRIP]]
; CHECK-NEXT: %g = getelementptr i8, ptr [[P_STRIP]], i64 8
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: %derived = load volatile i8, ptr %g
entry:
  ; Important: this strip is already the canonical conversion after %p.
  %p.s = call ptr @__ptsan_strip(ptr %p)
  %direct = load volatile i8, ptr %p.s, align 1

  ; Important: hoisting this second strip to %p should reuse %p.s.
  %g = getelementptr i8, ptr %p, i64 8
  %g.s = call ptr @__ptsan_strip(ptr %g)
  %derived = load volatile i8, ptr %g.s, align 1

  %direct.wide = zext i8 %direct to i16
  %derived.wide = zext i8 %derived to i16
  %sum = add i16 %direct.wide, %derived.wide
  ret i16 %sum
}
