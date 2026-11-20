;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=optimization -verify-each %s | FileCheck %s
; RUN: %opt_ptsan -verify-each %s -o /dev/null

target triple = "x86_64-unknown-linux-gnu"

; __cxa_begin_catch returns allocated storage, which __cxa_end_catch can free.
; Its result must not take the unfreeable-base shortcut around call barriers.
define i32 @catch_lifetime(ptr %exception) {
; CHECK-LABEL: define i32 @catch_lifetime(
; CHECK: call ptr @__cxa_begin_catch
; CHECK-NOT: @__ptsan_load_
; CHECK: call void @__cxa_end_catch()
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: load volatile i32
  %p = call ptr @__cxa_begin_catch(ptr %exception)
  call void @__cxa_end_catch()
  %value = load volatile i32, ptr %p, align 4
  ret i32 %value
}

declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()
