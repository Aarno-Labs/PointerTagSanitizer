;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-nochecks %s | FileCheck %s --implicit-check-not=optnone
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize=false %s | FileCheck %s --implicit-check-not=optnone
;
; Only the generated initializer replay should skip backend optimization.
; Its pointer stores must still be rewritten through the tagged global proxies.

target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

@target = global i32 42
@slot = global ptr @target
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @user_constructor, ptr null }
]

define void @user_constructor() noinline {
  %v = load volatile i32, ptr @target
  %next = add i32 %v, 1
  store volatile i32 %next, ptr @target
  ret void
}

define i32 @application(i32 %x) {
  %next = add i32 %x, 1
  ret i32 %next
}

; CHECK-LABEL: define void @user_constructor()
; CHECK-LABEL: define i32 @application(
; CHECK-LABEL: define internal void @__ptsan_init_tagged_globals()
; CHECK-LABEL: define internal void @__ptsan_init_extern_globals()
; CHECK: ; Function Attrs: {{.*}}noinline{{.*}}optnone
; CHECK-NEXT: define internal void @__ptsan_init_global_initializer_pointers() #[[ATTRS:[0-9]+]]
; CHECK: [[TAGGED:%.*]] = load ptr, ptr @target.ptsan.ptr
; CHECK: store ptr [[TAGGED]], ptr @slot
; CHECK: ret void
; CHECK: attributes #[[ATTRS]] = { {{.*}}noinline{{.*}}optnone{{.*}} }
