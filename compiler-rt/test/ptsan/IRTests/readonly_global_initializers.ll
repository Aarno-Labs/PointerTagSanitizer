;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

@target = global [4 x i32] zeroinitializer
@slot = constant ptr @target
@scalar = constant i32 7
@tls_slot = thread_local global ptr @target

; CHECK: $__ptsan_seal_global_initializers = comdat any
; CHECK: @slot = global ptr @target, section ".ptsan_init_ro"
; CHECK: @scalar = constant i32 7
; CHECK: @tls_slot = thread_local global ptr @target
; CHECK-DAG: @__ptsan_init_ro_start = external hidden global i8
; CHECK-DAG: @__ptsan_init_ro_end = external hidden global i8
; CHECK: { i32 3, ptr @__ptsan_seal_global_initializers, ptr @__ptsan_seal_global_initializers }

; CHECK-LABEL: define internal void @__ptsan_init_tagged_globals(
; CHECK: call i16 @ptsan_get_next_global_id(ptr @slot, i64 8)
; CHECK: call i16 @ptsan_get_constant_global_id(ptr @scalar, i64 4)

; CHECK-LABEL: define internal void @__ptsan_init_global_initializer_pointers(
; CHECK: load ptr, ptr @target.ptsan.ptr
; CHECK: store ptr

; CHECK-LABEL: define linkonce_odr hidden void @__ptsan_seal_global_initializers() comdat
; CHECK: call void @ptsan_protect_global_initializers(ptr @__ptsan_init_ro_start, ptr @__ptsan_init_ro_end)
; CHECK-NEXT: ret void
