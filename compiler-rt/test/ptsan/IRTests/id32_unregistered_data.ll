;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; Disabling global instrumentation leaves this data symbol without an ID.
; Unlike a function symbol or null, it must not bypass strict encoding just
; because its ID is constant zero. Non-strict mode retains raw passthrough.
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-instrument-globals=false -ptsan-strict-mode -ptsan-reoptimize=false %s | FileCheck %s --check-prefix=STRICT
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-instrument-globals=false -ptsan-reoptimize=false %s | FileCheck %s --check-prefix=NONSTRICT

target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

@unregistered = external global i8

define ptr @return_data() {
; STRICT-LABEL: define ptr @return_data(
; STRICT: sub i64 ptrtoint (ptr @unregistered to i64),
; STRICT: ret ptr %
; NONSTRICT-LABEL: define ptr @return_data(
; NONSTRICT: ret ptr @unregistered
  ret ptr @unregistered
}
