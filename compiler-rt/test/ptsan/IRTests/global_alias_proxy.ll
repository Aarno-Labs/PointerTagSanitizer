;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=late-optimization %s | FileCheck %s
;
; Proxy linkage/visibility must match the alias, including for TLS IDs.
; An interposable alias must not acquire its fallback's static bounds.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

@fallback = internal global i32 17
@overridden = weak hidden alias i32, ptr @fallback
@tls_fallback = internal thread_local global i32 17
@tls_overridden = weak hidden thread_local alias i32, ptr @tls_fallback

; CHECK-DAG: @overridden.ptsan.ptr = weak hidden alias ptr, ptr @fallback.ptsan.ptr
; CHECK-DAG: @tls_overridden.ptsan.id = weak hidden thread_local alias {{i16|i32}}, ptr @tls_fallback.ptsan.id

define i32 @read_overridden() {
  %value = load volatile i32, ptr @overridden
  ret i32 %value
}

; CHECK-LABEL: define i32 @read_overridden()
; CHECK-NOT: @__ptsan_static_
; CHECK: @__ptsan_load_span
; CHECK-NOT: @__ptsan_static_
; CHECK: @__ptsan_access

declare ptr @llvm.threadlocal.address.p0(ptr)

define ptr @get_tls_overridden() {
  %ptr = call ptr @llvm.threadlocal.address.p0(ptr @tls_overridden)
  ret ptr %ptr
}
