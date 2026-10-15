;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %clang_raw -x ir -fsanitize=ptsan -O2 \
; RUN:   -mllvm -ptsan-preanalyze-loops=1 \
; RUN:   -mllvm -ptsan-stop-after=metadata-lowering \
; RUN:   -S -emit-llvm -o - %s | FileCheck %s
;
; The unknown operand bundle keeps the all-false masked load present until
; loop preanalysis. It performs no memory access and must not contribute the
; span between its per-iteration pointer values to the loop summary. The
; volatile scalar load gives the loop a real four-byte range to summarize.

target triple = "x86_64-unknown-linux-gnu"

declare <4 x i32> @llvm.masked.load.v4i32.p0(ptr, i32 immarg, <4 x i1>,
                                             <4 x i32>)

define void @zero_mask_does_not_create_loop_range(ptr %base) {
; CHECK-LABEL: define void @zero_mask_does_not_create_loop_range(
; CHECK: %[[LAST:.*]] = getelementptr i8, ptr %base, i64 3
; CHECK: %[[MIN:.*]] = select i1 {{.*}}, ptr %base, ptr %[[LAST]]
; CHECK: call void (...) @__ptsan_loop_preanalysis({{.*}}ptr %[[MIN]], i64 4)
; CHECK-NOT: @__ptsan_loop_preanalysis
; CHECK: loop:
; CHECK: load volatile i8
; CHECK: call <4 x i32> @llvm.masked.load.v4i32.p0({{.*}}<4 x i1> zeroinitializer
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr i8, ptr %base, i64 %i
  %value = load volatile i8, ptr %p
  %far = getelementptr i8, ptr %p, i64 4096
  %unused = call <4 x i32> @llvm.masked.load.v4i32.p0(
      ptr %far, i32 4, <4 x i1> zeroinitializer, <4 x i32> poison) [ "keep"() ]
  %next = add nuw i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.disable"}
