;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=lowering %s | FileCheck %s

; A PHI may have multiple entries for one predecessor when that predecessor has
; multiple CFG edges to the PHI's block. Those entries must use the same value.
; PTSan's post-lowering cleanup must reunify the expanded values before the
; function is verified.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@g = global i32 0

define i64 @duplicate_edge_phi(i32 %x) {
; CHECK-LABEL: define i64 @duplicate_edge_phi(
; CHECK: entry:
; CHECK-NOT: = ptrtoint ptr @g to i64
; CHECK: [[P:%[A-Za-z0-9_.]+]] = ptrtoint ptr {{%[A-Za-z0-9_.]+}} to i64
; CHECK: switch i32 %x, label %join
; CHECK: join:
; CHECK-NEXT: ret i64 [[P]]
entry:
  ; Keep an ordinary access to @g alongside the constant-expression PHI uses.
  ; The PHI itself is the interesting use: all three duplicate edges must be
  ; redirected to the same materialized tagged value.
  %value = load volatile i32, ptr @g
  switch i32 %x, label %join [
    i32 0, label %join
    i32 1, label %join
  ]

join:
  %p = phi i64 [ ptrtoint (ptr @g to i64), %entry ],
               [ ptrtoint (ptr @g to i64), %entry ],
               [ ptrtoint (ptr @g to i64), %entry ]
  ret i64 %p
}

; InstCombine can fold the preceding PHI because all of its incoming values are
; equal. It cannot repair the general case where only the values on duplicate
; edges are equal.
define i64 @duplicate_edge_phi_mixed(i32 %x, i1 %cond) {
; CHECK-LABEL: define i64 @duplicate_edge_phi_mixed(
; CHECK: entry:
; CHECK: [[P:%[A-Za-z0-9_.]+]] = ptrtoint ptr {{%[A-Za-z0-9_.]+}} to i64
; CHECK: switch i32 %x, label %join
; CHECK: join:
; CHECK: %p = phi i64 [ [[P]], %entry ], [ [[P]], %entry ], [ [[P]], %entry ], [ 7, %other ]
start:
  %value = load volatile i32, ptr @g
  br i1 %cond, label %entry, label %other

entry:
  switch i32 %x, label %join [
    i32 0, label %join
    i32 1, label %join
  ]

other:
  br label %join

join:
  %p = phi i64 [ ptrtoint (ptr @g to i64), %entry ],
               [ ptrtoint (ptr @g to i64), %entry ],
               [ ptrtoint (ptr @g to i64), %entry ],
               [ 7, %other ]
  ret i64 %p
}
