;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-reoptimize=false %s | FileCheck %s

target triple = "x86_64-unknown-linux-musl"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

@value = global i32 42
@slot = global ptr @value
@tls = thread_local global i32 17

define i32 @application() {
  %v = load volatile i32, ptr @tls
  ret i32 %v
}

; CHECK-NOT: @llvm.global_ctors =
; CHECK: @__ptsan_global_init = internal constant { ptr, ptr, ptr } { ptr @__ptsan_init_extern_globals, ptr @__ptsan_init_tagged_globals, ptr @__ptsan_init_global_initializer_pointers }, section "ptsan_global_init", align 8
; CHECK: @llvm.used = {{.*}}@__ptsan_global_init
; CHECK-LABEL: define i32 @application()
; CHECK: call void @ptsan_validate_function_start()
; CHECK-LABEL: define internal void @__ptsan_init_tagged_globals()
; CHECK-NOT: call void @__ptsan_init_tagged_tls
; CHECK: ret void
; CHECK-LABEL: define internal void @__ptsan_init_extern_globals()
; CHECK: call void @ptsan_register_tls(ptr @__ptsan_init_extern_tls, ptr @__ptsan_init_tagged_tls, ptr @__ptsan_destroy_tls)
; CHECK-NOT: call void @__ptsan_init_extern_tls
; CHECK-NOT: call void @ptsan_enable_libc_instr
; CHECK: ret void
; CHECK-LABEL: define internal void @__ptsan_init_global_initializer_pointers()
; CHECK-NOT: call void @ptsan_validate_function_start
; CHECK: load ptr, ptr @value.ptsan.ptr
; CHECK: store ptr
; CHECK: ret void
; CHECK: "ptsan-pre-tls"
