;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DISCOVERY
; RUN: %opt_ptsan -ptsan-stop-after=lowering %s | FileCheck %s --check-prefix=LOWER
; RUN: %opt_ptsan %s -o /dev/null
;
; Discovery creates the exceptional exits before boundary helpers are lowered.
; Helpers which run after a call must use the invoke's normal destination.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @dlsym(ptr, ptr)
declare void @makecontext(ptr, ptr, i32, ...)

define ptr @lookup() {
entry:
  %name = alloca [8 x i8]
  %result = call ptr @dlsym(ptr null, ptr %name)
  ret ptr %result
}

; DISCOVERY-LABEL: define ptr @lookup()
; DISCOVERY: invoke ptr @dlsym(
; DISCOVERY: call void (...) @__ptsan_function_exit()
; DISCOVERY-NEXT: ret ptr
; DISCOVERY: landingpad
; DISCOVERY: call void (...) @__ptsan_function_exit()
; DISCOVERY-NEXT: resume

; LOWER-LABEL: define ptr @lookup()
; LOWER: [[OLD:%.*]] = load i32, ptr @ptsan_in_libc
; LOWER: store i32 {{.*}}, ptr @ptsan_in_libc
; LOWER: invoke ptr @dlsym(
; LOWER-NEXT: to label %[[NORMAL:[^ ]+]] unwind label
; LOWER: [[NORMAL]]:
; LOWER: store i32 [[OLD]], ptr @ptsan_in_libc

define void @context(ptr %fn) {
entry:
  %ctx = alloca [1024 x i8]
  call void (ptr, ptr, i32, ...) @makecontext(ptr %ctx, ptr %fn, i32 0)
  ret void
}

; LOWER-LABEL: define void @context(
; LOWER: call void (ptr, ptr, i32, ...) @ptsan_strip_makecontext_before(
; LOWER: invoke void (ptr, ptr, i32, ...) @makecontext(
; LOWER-NEXT: to label %[[NORMAL:[^ ]+]] unwind label
; LOWER: [[NORMAL]]:
; LOWER: call void (ptr, ptr, i32, ...) @ptsan_strip_makecontext_after(

; A byval source owns a frame ID; a TLS source reuses a thread-owned ID.
; Only the former needs an exceptional cleanup path.
@tls_value = thread_local global i32 0

declare ptr @llvm.threadlocal.address.p0(ptr)
declare void @consume(ptr)

define void @byval_source(ptr byval(i32) %value) {
entry:
  call void @consume(ptr %value)
  ret void
}

; DISCOVERY-LABEL: define void @byval_source(
; DISCOVERY: call ptr @__ptsan_source(ptr %value)
; DISCOVERY: invoke void @consume(
; DISCOVERY: call void (...) @__ptsan_function_exit()
; DISCOVERY-NEXT: ret void
; DISCOVERY: landingpad
; DISCOVERY: call void (...) @__ptsan_function_exit()
; DISCOVERY-NEXT: resume

define void @tls_source() {
entry:
  %value = call ptr @llvm.threadlocal.address.p0(ptr @tls_value)
  call void @consume(ptr %value)
  ret void
}

; DISCOVERY-LABEL: define void @tls_source()
; DISCOVERY: call ptr @__ptsan_source(ptr %value)
; DISCOVERY: call void @consume(
; DISCOVERY: call void (...) @__ptsan_function_exit()
; DISCOVERY-NEXT: ret void
; DISCOVERY-NEXT: }
