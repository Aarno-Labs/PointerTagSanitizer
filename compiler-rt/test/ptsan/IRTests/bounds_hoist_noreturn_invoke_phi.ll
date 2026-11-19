; RUN: %opt_ptsan -ptsan-reoptimize=0 %s -S | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; A noreturn invoke can free the pointee and then unwind. Bounds loads must
; stay on the exceptional continuation even when there is no later blocking
; call. The pointer PHI makes the invoke the initial hoisting candidate.

; CHECK-LABEL: define void @noreturn_invoke_phi(
; CHECK: merge:
; CHECK: phi ptr
; CHECK-NOT: load i64, ptr
; CHECK: invoke void @noreturn_call()
; CHECK: lpad:
; CHECK-DAG: load i64, ptr
; CHECK-DAG: load i64, ptr
; CHECK: access:
; CHECK: load i8, ptr

define void @noreturn_invoke_phi(ptr %ptr, ptr %other, i1 %cond) personality ptr null {
entry:
  br i1 %cond, label %left, label %right

left:
  br label %merge

right:
  br label %merge

merge:
  %base = phi ptr [ %ptr, %left ], [ %other, %right ]
  invoke void @noreturn_call()
          to label %unreachable unwind label %lpad

unreachable:
  unreachable

lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  br label %access

access:
  %value = load i8, ptr %base, align 1
  call void @use(i8 %value)
  resume { ptr, i32 } %lp
}

declare void @noreturn_call() noreturn
declare void @use(i8)
