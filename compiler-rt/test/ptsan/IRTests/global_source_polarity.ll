;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s | \
; RUN:   FileCheck %s --check-prefixes=CHECK,TLS \
; RUN:   --implicit-check-not=tls_bytes.ptsan.ptr
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-no-opts \
; RUN:   -ptsan-stop-after=lowering %s | FileCheck %s \
; RUN:   --check-prefixes=CHECK,TLS --implicit-check-not=tls_bytes.ptsan.ptr
;
; A global has two representations available without a conversion: the raw
; GlobalValue is stripped, while its proxy contains the tagged pointer. Global
; lowering should choose between them for each use instead of replacing every
; use with the tagged proxy.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

@bytes = external global [16 x i8]
@scalar = external global i8
@tls_bytes = external thread_local global [16 x i8]

declare void @escape(ptr)
declare ptr @llvm.threadlocal.address.p0(ptr)

define i8 @stripped_only() {
entry:
  %value = load volatile i8, ptr @bytes, align 1
  ret i8 %value
}

; CHECK-LABEL: define i8 @stripped_only(
; CHECK-NOT: @bytes.ptsan.ptr
; CHECK-NOT: @llvm.ptrmask
; CHECK: %value = load volatile i8, ptr @bytes
; CHECK-NOT: @llvm.ptrmask
; CHECK: ret i8 %value

define void @tagged_only() {
entry:
  call void @escape(ptr @bytes)
  ret void
}

; CHECK-LABEL: define void @tagged_only(
; CHECK: [[TAGGED:%.*]] = load ptr, ptr @bytes.ptsan.ptr
; CHECK-NOT: @llvm.ptrmask
; CHECK: call void @escape(ptr [[TAGGED]])
; CHECK-NOT: @llvm.ptrmask
; CHECK: ret void

define void @tagged_scalar() {
entry:
  call void @escape(ptr @scalar)
  ret void
}

; CHECK-LABEL: define void @tagged_scalar(
; CHECK: [[SCALAR:%.*]] = load ptr, ptr @scalar.ptsan.ptr
; CHECK-NOT: @llvm.ptrmask
; CHECK: call void @escape(ptr [[SCALAR]])
; CHECK-NOT: @llvm.ptrmask
; CHECK: ret void

define i8 @mixed() {
entry:
  %value = load volatile i8, ptr @bytes, align 1
  call void @escape(ptr @bytes)
  ret i8 %value
}

; CHECK-LABEL: define i8 @mixed(
; CHECK: [[TAGGED:%.*]] = load ptr, ptr @bytes.ptsan.ptr
; CHECK-NOT: @llvm.ptrmask
; CHECK: %value = load volatile i8, ptr @bytes
; CHECK: call void @escape(ptr [[TAGGED]])
; CHECK-NOT: @llvm.ptrmask
; CHECK: ret i8 %value

; TLS uses a per-thread ID, not a tagged-pointer proxy. Keep the intrinsic's
; address raw and attach that ID to the pointer passed to escape, with or
; without optimizations.
define void @tls_tagged() {
entry:
  %address = call ptr @llvm.threadlocal.address.p0(ptr @tls_bytes)
  call void @escape(ptr %address)
  ret void
}

; TLS-LABEL: define void @tls_tagged(
; TLS: [[ADDRESS:%.*]] = call ptr @llvm.threadlocal.address.p0(ptr @tls_bytes)
; TLS: [[ID:%.*]] = load i16, ptr @tls_bytes.ptsan.id
; TLS: [[ID64:%.*]] = zext i16 [[ID]] to i64
; TLS: [[RAW:%.*]] = ptrtoint ptr [[ADDRESS]] to i64
; TLS: [[TAG:%.*]] = shl i64 [[ID64]], 48
; TLS: [[TAGGED_INT:%.*]] = or i64 [[RAW]], [[TAG]]
; TLS: [[TAGGED:%.*]] = inttoptr i64 [[TAGGED_INT]] to ptr
; TLS: call void @escape(ptr [[TAGGED]])
; TLS: ret void
