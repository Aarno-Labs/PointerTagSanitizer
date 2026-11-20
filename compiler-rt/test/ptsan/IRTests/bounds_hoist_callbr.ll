;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=optimization -verify-each %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-strict-mode -ptsan-stop-after=optimization -verify-each %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-strict-mode -ptsan-runtime-validate -verify-each %s -o /dev/null

; PHP's asm-goto arithmetic helpers lower to callbr. Like invoke, callbr
; is a blocking call with no legal insertion point after it in its block.
; Exercise both candidate searches: the base's block and an intermediate
; dominator. Bounds must be reloaded after the asm on either successor.

target triple = "x86_64-unknown-linux-gnu"

define void @target_block(ptr %slot) {
; CHECK-LABEL: define void @target_block(
; CHECK: %p = load ptr
; CHECK-NOT: @__ptsan_load_
; CHECK: callbr void asm
; CHECK: fallthrough:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: store i64 0
; CHECK: taken:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: store i64 1
entry:
  %p = load ptr, ptr %slot
  callbr void asm sideeffect "", "!i,~{memory}"()
          to label %fallthrough [label %taken]
fallthrough:
  store i64 0, ptr %p
  ret void
taken:
  store i64 1, ptr %p
  ret void
}

define void @intermediate_block(ptr %p, i1 %skip) {
; CHECK-LABEL: define void @intermediate_block(
; CHECK-NOT: @__ptsan_load_
; CHECK: callbr void asm
; CHECK: fallthrough:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: store i64 0
; CHECK: taken:
; CHECK-DAG: call i64 @__ptsan_load_base
; CHECK-DAG: call i64 @__ptsan_load_span
; CHECK: store i64 1
entry:
  br i1 %skip, label %exit, label %asm
asm:
  callbr void asm sideeffect "", "!i,~{memory}"()
          to label %fallthrough [label %taken]
fallthrough:
  store i64 0, ptr %p
  ret void
taken:
  store i64 1, ptr %p
  ret void
exit:
  ret void
}
