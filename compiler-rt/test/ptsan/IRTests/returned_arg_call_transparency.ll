; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; getArgumentAliasingToReturnedPointer() is an alias/provenance fact, not a
; rewriteable dataflow contract. An ordinary returned-argument call still
; consumes its pointer argument, so that argument must remain tagged even when
; the returned value is later stripped.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @ret_arg(ptr returned)
declare ptr @__ptsan_strip(ptr)
declare ptr @__ptsan_tagged(ptr)

define i8 @returned_arg_call_result_strip(ptr %p, i64 %n) {
; CHECK-LABEL: define i8 @returned_arg_call_result_strip(
; CHECK: entry:
; CHECK-NEXT: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %p)
; CHECK-NEXT: %r = call ptr @ret_arg(ptr [[TAGGED]])
; CHECK-NEXT: [[R_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %r)
; CHECK: %g = getelementptr i8, ptr [[R_STRIP]], i64 %iv
entry:
  %r = call ptr @ret_arg(ptr %p)
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %r, i64 %iv
  %s = call ptr @__ptsan_strip(ptr %g)
  %v = load volatile i8, ptr %s, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %v
}
