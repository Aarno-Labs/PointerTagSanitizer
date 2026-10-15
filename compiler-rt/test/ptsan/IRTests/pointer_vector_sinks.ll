;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks \
; RUN:   -ptsan-runtime-validate -ptsan-runtime-validate-arith \
; RUN:   -ptsan-stop-after=lowering %s | FileCheck %s

; Pointer vectors are stripped before reaching opaque sinks. Arithmetic
; validation has a scalar runtime interface, so vector comparisons must not
; create malformed calls to it.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define <4 x i1> @compare(<4 x ptr> %lhs, <4 x ptr> %rhs) {
; CHECK-LABEL: define <4 x i1> @compare(
; CHECK-DAG: [[RAW_LHS:%.*]] = call <4 x ptr> @llvm.ptrmask.v4p0.v4i64(<4 x ptr> %lhs,
; CHECK-DAG: [[RAW_RHS:%.*]] = call <4 x ptr> @llvm.ptrmask.v4p0.v4i64(<4 x ptr> %rhs,
; CHECK-NOT: call void @ptsan_validate_arithmetic
; CHECK: [[CMP:%.*]] = icmp ult <4 x ptr> [[RAW_LHS]], [[RAW_RHS]]
; CHECK-NEXT: ret <4 x i1> [[CMP]]
entry:
  %cmp = icmp ult <4 x ptr> %lhs, %rhs
  ret <4 x i1> %cmp
}

define void @inline_asm(<4 x ptr> %pointers) {
; CHECK-LABEL: define void @inline_asm(
; CHECK: [[RAW:%.*]] = call <4 x ptr> @llvm.ptrmask.v4p0.v4i64(<4 x ptr> %pointers,
; CHECK: call void asm sideeffect "", "v"(<4 x ptr> [[RAW]])
; CHECK-NEXT: ret void
entry:
  call void asm sideeffect "", "v"(<4 x ptr> %pointers)
  ret void
}
