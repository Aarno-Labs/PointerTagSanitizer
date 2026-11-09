;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; SCEV can rebuild an address with a byte GEP from null. Normalize this to a
; cast regardless of where the integer came from, so PTSan does not treat an
; encoded pointer as stripped. Narrow indices and scaled GEPs are left alone.
; RUN: %opt_ptsan -ptsan-stop-after=prepare %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=prepare %s | FileCheck %s

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

define ptr @from_integer(i64 %bits) {
; CHECK-LABEL: define ptr @from_integer(
; CHECK: %result = inttoptr i64 %bits to ptr
; CHECK: ret ptr %result
  %result = getelementptr i8, ptr null, i64 %bits
  ret ptr %result
}

define ptr @from_cancelled_base(ptr %base, i64 %bits) {
; CHECK-LABEL: define ptr @from_cancelled_base(
; CHECK: %result = inttoptr i64 %bits to ptr
; CHECK: ret ptr %result
  %base_bits = ptrtoint ptr %base to i64
  %negative = sub i64 0, %base_bits
  %zero = getelementptr i8, ptr %base, i64 %negative
  %result = getelementptr i8, ptr %zero, i64 %bits
  ret ptr %result
}

define ptr @narrow(i32 %bits) {
; CHECK-LABEL: define ptr @narrow(
; CHECK: getelementptr i8, ptr null, i32 %bits
  %result = getelementptr i8, ptr null, i32 %bits
  ret ptr %result
}

define ptr @scaled(i64 %bits) {
; CHECK-LABEL: define ptr @scaled(
; CHECK: getelementptr i64, ptr null, i64 %bits
  %result = getelementptr i64, ptr null, i64 %bits
  ret ptr %result
}
