; RUN: %opt_ptsan -ptsan-stop-after=late-optimization %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-stop-after=lowering %s -o /dev/null
;
; Global strip-ID placement chooses a stripped loop-carried PHI whose incoming
; pointers come from different base objects. Its check still needs the matching
; incoming ID, represented by a parallel ID PHI.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.node = type { i32, i64, ptr, ptr, i16, ptr, ptr, i64, i64 }

define ptr @phi_multibase_load(ptr %hi, i64 %stride, ptr %base) {
entry:
  br label %loop

loop:
  %p = phi ptr [ %next, %loop ], [ %merge, %join ], [ null, %entry ]
  %v = load i16, ptr %p, align 8
  %next = getelementptr %struct.node, ptr %base, i64 %stride
  %c1 = icmp ult ptr %next, %hi
  br i1 false, label %loop, label %join

dead:
  br label %join

join:
  %merge = phi ptr [ null, %dead ], [ null, %loop ]
  %c2 = icmp eq ptr %merge, %hi
  br label %loop
}

; CHECK-LABEL: define ptr @phi_multibase_load(
; CHECK: [[P_ID:%.*]] = phi i64
; CHECK: %p = phi ptr
; CHECK: call void (...) @__ptsan_access(i64 [[P_ID]],
