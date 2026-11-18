; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=late-optimization %s | FileCheck %s
;
; Keep coverage of the pointer-splat form independently of SLP's choice to emit
; scalar-base/vector-index GEPs in LLVM 23. Both the bounded scalar prefix and
; the constant vector offsets fit in the ID32 payload without stripping.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@objects = global [256 x i32] zeroinitializer, align 16

define void @scalar_splat(i32 %index, ptr %output) {
; CHECK-LABEL: define void @scalar_splat(
; CHECK: [[GLOBAL:%.*]] = load ptr, ptr @objects.ptsan.ptr
; CHECK: %start = getelementptr inbounds [256 x i32], ptr [[GLOBAL]], i64 0, i64 %offset, !ptsan.tagged
; CHECK: %splat = shufflevector <4 x ptr> {{.*}}, <4 x ptr> poison, <4 x i32> zeroinitializer
; CHECK-NOT: call <4 x ptr> @__ptsan_strip_id32
; CHECK: %pointers = getelementptr i8, <4 x ptr> %splat, <4 x i64> <i64 4, i64 12, i64 20, i64 28>, !ptsan.tagged
; CHECK-NOT: call <4 x ptr> @__ptsan_tag_id32
; CHECK: store <4 x ptr>
; CHECK: ret void
  %bounded = and i32 %index, 15
  %offset = zext i32 %bounded to i64
  %start = getelementptr inbounds [256 x i32], ptr @objects, i64 0, i64 %offset
  %inserted = insertelement <4 x ptr> poison, ptr %start, i64 0
  %splat = shufflevector <4 x ptr> %inserted, <4 x ptr> poison, <4 x i32> zeroinitializer
  %pointers = getelementptr i8, <4 x ptr> %splat, <4 x i64> <i64 4, i64 12, i64 20, i64 28>
  store <4 x ptr> %pointers, ptr %output, align 8
  ret void
}
