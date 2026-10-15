;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s | \
; RUN:   FileCheck %s --implicit-check-not='= available_externally ' \
; RUN:   --implicit-check-not=.ptsan_init_ro
;
; LTO vtable initializers can be optimization-only copies of definitions in a
; library. Their proxies must be emitted, but these copies must not be assigned
; allocation IDs or replayed into the library's actual read-only storage.

target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

@target = external global i32
@borrowed = available_externally constant ptr @target
@owned_slot = global ptr @borrowed

declare void @escape(ptr)

define void @use_borrowed() {
  call void @escape(ptr @borrowed)
  ret void
}

; CHECK: @borrowed = external constant ptr
; CHECK: @borrowed.ptsan.ptr = weak global ptr @borrowed

; CHECK-LABEL: define void @use_borrowed(
; CHECK: load ptr, ptr @borrowed.ptsan.ptr
; CHECK: call void @escape(

; CHECK-LABEL: define internal void @__ptsan_init_tagged_globals()
; CHECK-NOT: @borrowed
; CHECK: @ptsan_get_next_global_id(ptr @owned_slot,
; CHECK-NOT: @borrowed
; CHECK: ret void

; CHECK-LABEL: define internal void @__ptsan_init_extern_globals()
; CHECK: store ptr @borrowed, ptr @borrowed.ptsan.ptr

; CHECK-LABEL: define internal void @__ptsan_init_global_initializer_pointers()
; CHECK: load ptr, ptr @borrowed.ptsan.ptr
; CHECK: store ptr {{.*}}, ptr @owned_slot
; CHECK-NOT: store
; CHECK: ret void
