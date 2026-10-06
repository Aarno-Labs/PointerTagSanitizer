;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; REQUIRES: x86_64-target-arch
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks -ptsan-stop-after=lowering %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks -ptsan-stop-after=lowering -ptsan-x86-inline-strip=0 %s | FileCheck %s --check-prefix=FALLBACK --implicit-check-not=asm
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-nochecks -ptsan-stop-after=lowering -mtriple=aarch64-unknown-linux-gnu %s | FileCheck %s --check-prefix=FALLBACK --implicit-check-not=asm

target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)
declare i64 @__ptsan_strip_i64(i64)

define i64 @integer_strip(i64 %value) #0 {
; FALLBACK-LABEL: define i64 @integer_strip(
; FALLBACK: and i64 %value, 281474976710655
; FALLBACK: ret i64
; CHECK-LABEL: define i64 @integer_strip(
; CHECK: [[RAW:%.*]] = call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 %value, i64 281474976710655) #[[ATTR:[0-9]+]]
; CHECK-NEXT: ret i64 [[RAW]]
  %raw = call i64 @__ptsan_strip_i64(i64 %value)
  ret i64 %raw
}

define <4 x ptr> @vector_strip(<4 x ptr> %value) #0 {
; CHECK-LABEL: define <4 x ptr> @vector_strip(
; CHECK-NOT: asm
; CHECK: [[RAW:%.*]] = call <4 x ptr> @llvm.ptrmask.v4p0.v4i64
; CHECK-NEXT: ret <4 x ptr> [[RAW]]
  %raw = call <4 x ptr> @__ptsan_strip(<4 x ptr> %value)
  ret <4 x ptr> %raw
}

define ptr @override_fast(ptr %value) #1 {
; CHECK-LABEL: define ptr @override_fast(
; CHECK: call i64 asm "andq $2, $0", "=r,0,r,~{flags}"
; CHECK: ret ptr
; FALLBACK-LABEL: define ptr @override_fast(
; FALLBACK: call ptr @llvm.ptrmask.p0.i64
; FALLBACK: ret ptr
  %raw = call ptr @__ptsan_strip(ptr %value)
  ret ptr %raw
}

define ptr @override_slow(ptr %value) #2 {
; CHECK-LABEL: define ptr @override_slow(
; CHECK: call i64 asm "andq $2, $0", "=r,0,r,~{flags}"
; CHECK: ret ptr
  %raw = call ptr @__ptsan_strip(ptr %value)
  ret ptr %raw
}

define ptr @generic_strip(ptr %value) {
; CHECK-LABEL: define ptr @generic_strip(
; CHECK: call i64 asm "andq $2, $0", "=r,0,r,~{flags}"
; CHECK: ret ptr
  %raw = call ptr @__ptsan_strip(ptr %value)
  ret ptr %raw
}

define ptr @null_strip() #0 {
; CHECK-LABEL: define ptr @null_strip(
; CHECK-NEXT: ret ptr null
  %raw = call ptr @__ptsan_strip(ptr null)
  ret ptr %raw
}

attributes #0 = { "target-cpu"="haswell" "target-features"="-bmi2" }
attributes #1 = { "target-cpu"="haswell" "target-features"="-bmi2,+fast-bextr" }
attributes #2 = { "target-cpu"="znver1" "target-features"="-bmi2,-fast-bextr" }
; CHECK: attributes #[[ATTR]] = { nounwind willreturn memory(none) }
