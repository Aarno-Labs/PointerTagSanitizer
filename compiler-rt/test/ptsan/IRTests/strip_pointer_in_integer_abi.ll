;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; PTSan preserves IDs across ptrtoint and the generic PTSan musl dispatcher
; boundary, including when it carries a fixed-width syscall executor.
; __syscall_cp is C dispatch code and must also preserve IDs until checking
; at the syscall boundary. Inline assembly must receive raw addresses.
;
; RUN: %opt_ptsan %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i64 @__syscall_cp(i64, i64, i64, i64, i64, i64, i64)
declare i64 @ptsan_musl_executor(i64, i64, i64, i64, i64, i64, i64)
declare i64 @ptsan_musl_syscall(ptr, i64, i64, i64, i64, i64, i64, i64)

define i64 @musl_syscall_cp(ptr %p) {
entry:
  %as_int = ptrtoint ptr %p to i64
  %result = call i64 @__syscall_cp(i64 1, i64 %as_int, i64 0, i64 0,
                                   i64 0, i64 0, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @musl_syscall_cp(
; CHECK: [[CP_INT:%.*]] = ptrtoint ptr %p to i64
; CHECK-NOT: and i64 [[CP_INT]]
; CHECK-NOT: asm "andq
; CHECK: call i64 @__syscall_cp(i64 1, i64 [[CP_INT]], i64 0, i64 0,

define i64 @ptsan_musl_dispatch(ptr %p) {
entry:
  %as_int = ptrtoint ptr %p to i64
  %result = call i64 @ptsan_musl_syscall(
      ptr @ptsan_musl_executor, i64 1, i64 1, i64 %as_int, i64 4,
      i64 0, i64 0, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @ptsan_musl_dispatch(
; CHECK: [[AS_INT:%.*]] = ptrtoint ptr %p to i64
; CHECK-NOT: and i64 [[AS_INT]]
; CHECK-NOT: asm "andq
; CHECK: call i64 @ptsan_musl_syscall({{.*}}i64 [[AS_INT]]

define void @inline_asm(ptr %p) {
entry:
  %as_int = ptrtoint ptr %p to i64
  call void asm sideeffect "", "r"(i64 %as_int)
  ret void
}

; CHECK-LABEL: define void @inline_asm(
; CHECK: [[ASM_INT:%.*]] = ptrtoint ptr %p to i64
; CHECK: [[ASM_RAW:%.*]] = {{.*}}call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[ASM_INT]], i64 281474976710655)
; CHECK: call void asm sideeffect "", "r"(i64 [[ASM_RAW]])
