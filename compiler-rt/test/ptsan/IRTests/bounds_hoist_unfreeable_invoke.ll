;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=optimization -verify-each %s | FileCheck %s
; RUN: %opt_ptsan -verify-each %s -o /dev/null

; The unfreeable-base fast path used to return {invoke, AFTER} without going
; through the CFG search that excludes invalid insertion points.
target triple = "x86_64-unknown-linux-gnu"
declare ptr @__cxa_begin_catch(ptr)

define i32 @unfreeable_invoke(ptr %exn) personality ptr null {
; CHECK-LABEL: define i32 @unfreeable_invoke(
; CHECK: entry:
; CHECK-NOT: @__ptsan_load_
; CHECK: %p = invoke ptr @__cxa_begin_catch
; CHECK: normal:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: load i32
; CHECK: unwind:
; CHECK-NOT: @__ptsan_load_
; CHECK: resume
entry:
  %p = invoke ptr @__cxa_begin_catch(ptr %exn)
          to label %normal unwind label %unwind
normal:
  %v = load i32, ptr %p
  ret i32 %v
unwind:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}
