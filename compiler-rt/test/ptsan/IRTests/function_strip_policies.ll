;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; Per-function arithmetic and comparison policies must affect only their named
; functions. Numeric comparisons exercise clear-ids-on-cmp: direct pointer
; comparisons are always stripped. These are ordinary, uninstrumented inputs.
; RUN: %opt_ptsan -ptsan-stop-after=discovery -ptsan-clear-ids-on-arith=0 -ptsan-clear-ids-on-cmp=0 -ptsan-clear-ids-on-arith-function=raw_sub -ptsan-clear-ids-on-cmp-function=raw_cmp %s | FileCheck %s

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

define i64 @raw_sub(ptr %p, ptr %q) {
; CHECK-LABEL: define i64 @raw_sub(
; CHECK: [[P:%.*]] = call i64 @__ptsan_strip_i64(i64 %pi
; CHECK: [[Q:%.*]] = call i64 @__ptsan_strip_i64(i64 %qi
; CHECK: sub i64 [[P]], [[Q]]
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %delta = sub i64 %pi, %qi
  ret i64 %delta
}

define i1 @raw_cmp(ptr %p, ptr %q) {
; CHECK-LABEL: define i1 @raw_cmp(
; CHECK: [[P:%.*]] = call ptr @__ptsan_strip(ptr %p
; CHECK: %pi = ptrtoint ptr [[P]] to i64
; CHECK: [[Q:%.*]] = call ptr @__ptsan_strip(ptr %q
; CHECK: %qi = ptrtoint ptr [[Q]] to i64
; CHECK: icmp ult i64 %pi, %qi
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %cmp = icmp ult i64 %pi, %qi
  ret i1 %cmp
}

define i64 @keep_sub(ptr %p, ptr %q) {
; CHECK-LABEL: define i64 @keep_sub(
; CHECK-NOT: call {{.*}}@__ptsan_strip
; CHECK: ret i64
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %delta = sub i64 %pi, %qi
  ret i64 %delta
}

define i1 @keep_cmp(ptr %p, ptr %q) {
; CHECK-LABEL: define i1 @keep_cmp(
; CHECK-NOT: call {{.*}}@__ptsan_strip
; CHECK: icmp ult i64 %pi, %qi
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %cmp = icmp ult i64 %pi, %qi
  ret i1 %cmp
}
