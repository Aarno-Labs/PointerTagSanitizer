;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; Function symbols must remain code addresses at tagged call/return boundaries
; and stripped comparison boundaries. They must not acquire allocation IDs or
; load allocation metadata. Literal null and sentinel values remain unchanged.
; RUN: %opt_ptsan -ptsan-reoptimize=false %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-reoptimize=false %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-reoptimize=false %s | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

declare void @callback()
declare void @consume(ptr)

define ptr @return_function() {
; CHECK-LABEL: define ptr @return_function(
; CHECK-NOT: @ptsan_bounds
; CHECK: ret ptr @callback
  ret ptr @callback
}

define void @pass_function() {
; CHECK-LABEL: define void @pass_function(
; CHECK-NOT: @ptsan_bounds
; CHECK: call void @consume(ptr @callback)
  call void @consume(ptr @callback)
  ret void
}

; The unknown argument may need decoding; the named function does not.
define i1 @compare_function(ptr %p) {
; CHECK-LABEL: define i1 @compare_function(
; CHECK: icmp eq ptr {{.*}}, @callback
  %same = icmp eq ptr %p, @callback
  ret i1 %same
}

define ptr @return_null() {
; CHECK-LABEL: define ptr @return_null(
; CHECK-NOT: @ptsan_bounds
; CHECK: ret ptr null
  ret ptr null
}

define ptr @return_sentinel() {
; CHECK-LABEL: define ptr @return_sentinel(
; CHECK-NOT: @ptsan_bounds
; CHECK: ret ptr inttoptr (i64 -1 to ptr)
  ret ptr inttoptr (i64 -1 to ptr)
}
