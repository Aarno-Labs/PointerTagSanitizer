; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; An existing strip of %p can also serve stripped users introduced by global
; hoisting. The optimizer should count and emit that conversion once rather than
; keeping a second strip below the GEP.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"


define i16 @existing_strip_is_shared_conversion(ptr %p) {
; CHECK-LABEL: define i16 @existing_strip_is_shared_conversion(
; CHECK: entry:
; CHECK: [[P_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p
; CHECK-NEXT: %direct = load volatile i8, ptr [[P_STRIP]]
; CHECK-NEXT: %g = getelementptr inbounds i8, ptr [[P_STRIP]], i64 8
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: %derived = load volatile i8, ptr %g
entry:
  ; Discovery creates the first strip for this direct load.
  %direct = load volatile i8, ptr %p, align 1

  ; Hoisting the derived load's strip should reuse the direct load's strip.
  %g = getelementptr inbounds i8, ptr %p, i64 8
  %derived = load volatile i8, ptr %g, align 1

  %direct.wide = zext i8 %direct to i16
  %derived.wide = zext i8 %derived to i16
  %sum = add i16 %direct.wide, %derived.wide
  ret i16 %sum
}
