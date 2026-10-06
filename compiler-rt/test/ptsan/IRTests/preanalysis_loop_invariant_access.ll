;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; XFAIL: *
; RUN: %clang_raw -x ir -fsanitize=ptsan -O2 -fno-vectorize \
; RUN:   -fno-slp-vectorize -fno-unroll-loops -S -emit-llvm -o - %s \
; RUN:   | FileCheck %s
;
; Loop preanalysis should handle an access whose pointer is invariant in the
; loop.  The volatile store keeps the access in the loop, while its block
; post-dominates the preheader and both the pointer and access size are already
; available there.  The complete accessed range is therefore simply [%ptr,
; %ptr + 4), and one preheader check can cover every iteration.
;
; This currently fails because getLoopAccessExtent() requires every pointer
; SCEV to contain an AddRecurrence.  The SCEV for %ptr is loop-invariant, so
; preanalysis rejects the access and records an incomplete loop instead.

define void @loop_invariant_access(ptr %ptr, i64 %count) {
entry:
  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %exit, label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  store volatile i32 0, ptr %ptr, align 4
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, %count
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; CHECK: !{!"ptsan.loop.bounds", i1 true
