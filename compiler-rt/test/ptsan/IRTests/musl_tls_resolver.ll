;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-reoptimize=false %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-strict-mode -ptsan-nochecks -ptsan-reoptimize=false %s | FileCheck %s

target triple = "x86_64-unknown-linux-musl"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

declare ptr @__tls_get_addr(ptr)

define ptr @explicit_resolver(ptr %index) {
  %result = call ptr @__tls_get_addr(ptr %index)
  ret ptr %result
}

; CHECK-LABEL: define ptr @explicit_resolver(
; CHECK-NOT: call ptr @__tls_get_addr(
; CHECK: call ptr @ptsan_musl_tls_get_addr(ptr %index)
; CHECK: ret ptr

; TLS references must remain TLS references for native backend lowering.
@tls = external thread_local global i32
define i32 @tls_access() {
  %value = load volatile i32, ptr @tls
  ret i32 %value
}
; CHECK-LABEL: define i32 @tls_access()
; CHECK: @tls
