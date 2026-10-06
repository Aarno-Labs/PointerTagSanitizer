;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-stop-after=metadata-lowering %s | \
; RUN:   FileCheck %s --check-prefix=META
; RUN: %opt_ptsan -ptsan-no-opts %s -o /dev/null

; Scalable masked accesses cannot use the fixed-lane scalar-envelope scan.
; Their ordinary vector ranges remain scalable through materialization. A
; contiguous access uses llvm.experimental.stepvector to construct its lane
; addresses, while a gather already supplies explicit lane pointers.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <vscale x 4 x i32> @llvm.masked.load.nxv4i32.p0(
    ptr, i32 immarg, <vscale x 4 x i1>, <vscale x 4 x i32>)
declare <vscale x 4 x i32> @llvm.masked.gather.nxv4i32.nxv4p0(
    <vscale x 4 x ptr>, i32 immarg, <vscale x 4 x i1>,
    <vscale x 4 x i32>)

define <vscale x 4 x i32> @contiguous(ptr %base,
                                      <vscale x 4 x i1> %mask) {
; META-LABEL: define <vscale x 4 x i32> @contiguous(
; META: [[LANES:%.*]] = call <vscale x 4 x i64> @llvm.experimental.stepvector.nxv4i64()
; META: [[OFFSETS:%.*]] = mul <vscale x 4 x i64> [[LANES]], {{.*}}
; META: [[POINTERS:%.*]] = getelementptr i8, ptr %base, <vscale x 4 x i64> [[OFFSETS]]
; META: call void (...) @__ptsan_access({{.*}}<vscale x 4 x ptr> [[POINTERS]], <vscale x 4 x i64> {{.*}}, i64 4, i32 1, i32 0, <vscale x 4 x i1> %mask)
entry:
  %value = call <vscale x 4 x i32> @llvm.masked.load.nxv4i32.p0(
      ptr %base, i32 4, <vscale x 4 x i1> %mask,
      <vscale x 4 x i32> poison)
  ret <vscale x 4 x i32> %value
}

define <vscale x 4 x i32> @gather(<vscale x 4 x ptr> %pointers,
                                  <vscale x 4 x i1> %mask) {
; META-LABEL: define <vscale x 4 x i32> @gather(
; META-NOT: llvm.experimental.stepvector
; META: call void (...) @__ptsan_access({{.*}}<vscale x 4 x ptr> %pointers, <vscale x 4 x i64> {{.*}}, i64 4, i32 1, i32 0, <vscale x 4 x i1> %mask)
entry:
  %value = call <vscale x 4 x i32> @llvm.masked.gather.nxv4i32.nxv4p0(
      <vscale x 4 x ptr> %pointers, i32 4, <vscale x 4 x i1> %mask,
      <vscale x 4 x i32> poison)
  ret <vscale x 4 x i32> %value
}
