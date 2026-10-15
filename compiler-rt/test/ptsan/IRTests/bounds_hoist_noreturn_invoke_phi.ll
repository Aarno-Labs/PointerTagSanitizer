; RUN: %opt_ptsan -ptsan-reoptimize=0 %s -S | FileCheck %s

; A noreturn invoke is not a blocking call, so it can be the first legal
; insertion point after a pointer PHI. Do not reject that candidate just
; because it is an invoke. The later blocking call still prevents the bounds
; loads from being hoisted above it.

; CHECK-LABEL: define void @noreturn_invoke_phi(
; CHECK: merge:
; CHECK: phi ptr
; CHECK: invoke void @noreturn_call()
; CHECK: lpad:
; CHECK: call void @may_free()
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
  call void @may_free()
  br label %access

access:
  %value = load i8, ptr %base, align 1
  call void @use(i8 %value)
  resume { ptr, i32 } %lp
}

declare void @noreturn_call() noreturn
declare void @may_free()
declare void @use(i8)
