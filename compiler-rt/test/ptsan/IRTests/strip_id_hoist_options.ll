; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=MINCUT
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization -ptsan-use-local-strip-id-hoist %s | FileCheck %s --check-prefix=LOCAL
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization -ptsan-no-strip-id-hoist %s | FileCheck %s --check-prefix=DISABLED
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization -ptsan-use-local-strip-id-hoist -ptsan-no-strip-id-hoist %s | FileCheck %s --check-prefix=DISABLED

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)

; Both optimizers hoist the loop strip. Disabling strip-ID hoisting leaves it
; on the derived pointer.
define i8 @loop_strip(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %loop ]
  %q = getelementptr i8, ptr %p, i64 %i
  %s = call ptr @__ptsan_strip(ptr %q)
  %v = load volatile i8, ptr %s
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %v
}

; MINCUT-LABEL: define i8 @loop_strip(
; MINCUT: [[BASE:%.*]] = call ptr @__ptsan_strip(ptr %p)
; MINCUT: %q = getelementptr i8, ptr [[BASE]], i64 %i
; MINCUT-NOT: call ptr @__ptsan_strip(ptr %q)

; LOCAL-LABEL: define i8 @loop_strip(
; LOCAL: [[BASE:%.*]] = call ptr @__ptsan_strip(ptr %p)
; LOCAL: %q = getelementptr i8, ptr [[BASE]], i64 %i
; LOCAL-NOT: call ptr @__ptsan_strip(ptr %q)

; DISABLED-LABEL: define i8 @loop_strip(
; DISABLED: %q = getelementptr i8, ptr %p, i64 %i
; DISABLED-NEXT: %s = call ptr @__ptsan_strip(ptr %q)

; Min-cut pays for one shared strip of %sub. The local optimizer leaves the
; independent leaf strips in place, which distinguishes the selector knob.
define void @shared_subbase_strip(ptr %p, ptr %slot) {
entry:
  %sub = getelementptr i8, ptr %p, i64 8
  store ptr %sub, ptr %slot
  %a = getelementptr i8, ptr %sub, i64 1
  %a.s = call ptr @__ptsan_strip(ptr %a)
  %a.v = load volatile i8, ptr %a.s
  %b = getelementptr i8, ptr %sub, i64 2
  %b.s = call ptr @__ptsan_strip(ptr %b)
  %b.v = load volatile i8, ptr %b.s
  ret void
}

; MINCUT-LABEL: define void @shared_subbase_strip(
; MINCUT: %sub = getelementptr i8, ptr %p, i64 8
; MINCUT-NEXT: [[SUB:%.*]] = call ptr @__ptsan_strip(ptr %sub)
; MINCUT: %a = getelementptr i8, ptr [[SUB]], i64 1
; MINCUT-NOT: call ptr @__ptsan_strip(ptr %a)
; MINCUT: %b = getelementptr i8, ptr [[SUB]], i64 2
; MINCUT-NOT: call ptr @__ptsan_strip(ptr %b)

; LOCAL-LABEL: define void @shared_subbase_strip(
; LOCAL: %a = getelementptr i8, ptr %sub, i64 1
; LOCAL-NEXT: %a.s = call ptr @__ptsan_strip(ptr %a)
; LOCAL: %b = getelementptr i8, ptr %sub, i64 2
; LOCAL-NEXT: %b.s = call ptr @__ptsan_strip(ptr %b)

; DISABLED-LABEL: define void @shared_subbase_strip(
; DISABLED: %a = getelementptr i8, ptr %sub, i64 1
; DISABLED-NEXT: %a.s = call ptr @__ptsan_strip(ptr %a)
; DISABLED: %b = getelementptr i8, ptr %sub, i64 2
; DISABLED-NEXT: %b.s = call ptr @__ptsan_strip(ptr %b)
