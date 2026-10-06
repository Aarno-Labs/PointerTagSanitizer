;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https:;llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-strict-mode -ptsan-no-opts -mtriple=aarch64-unknown-linux-musl %s | FileCheck %s --check-prefixes=COMMON,ARM
; RUN: %opt_ptsan -ptsan-strict-mode -ptsan-no-opts -mtriple=x86_64-unknown-linux-musl %s | FileCheck %s --check-prefixes=COMMON,X86
;
; The frame owns one ID per ABI backing region: three on AArch64, two on
; x86-64. Initialize immediately after va_start and retire at function exit.
; COMMON-LABEL: define void @reader(
; ARM: call void @ptsan_get_stack_ids_n({{.*}}i64 3)
; X86: call void @ptsan_get_stack_ids_n({{.*}}i64 2)
; COMMON: call void @llvm.va_start{{.*}}(ptr [[RAW:%[^)]+]])
; ARM-NEXT: call void @ptsan_init_va_list_aarch64(ptr [[RAW]], i64 {{.*}}, i64 {{.*}}, i64 {{.*}})
; X86-NEXT: call void @ptsan_init_va_list_x86_64(ptr [[RAW]], i64 {{.*}}, i64 {{.*}}, i64 176)
; ARM: call void @ptsan_free_stack_ids_n({{.*}}i64 3)
; X86: call void @ptsan_free_stack_ids_n({{.*}}i64 2)
; COMMON: ret void
;
; Both backends place the appended IR byval marker on the stack even when
; argument registers remain. It must not become an ordinary pointer argument.
; COMMON-LABEL: define void @caller(
; COMMON: call void (i32, ...) @callee(i32 1, i64 2, ptr {{.*}}byval([2 x i64]) @__ptsan_va_sentinel)

target datalayout = "e-p:64:64"

declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)
declare void @callee(i32, ...)

define void @reader(ptr %ap, ...) {
  call void @llvm.va_start(ptr %ap)
  call void @llvm.va_end(ptr %ap)
  ret void
}

define void @caller() {
  call void (i32, ...) @callee(i32 1, i64 2)
  ret void
}
