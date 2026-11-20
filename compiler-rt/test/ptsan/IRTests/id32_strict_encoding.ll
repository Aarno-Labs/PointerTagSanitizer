;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-reoptimize=false %s | FileCheck %s --check-prefix=MARKERLESS
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode=0 -ptsan-reoptimize=false %s | FileCheck %s --check-prefix=MARKER
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-support-indicator-bit -ptsan-reoptimize=false %s | FileCheck %s --check-prefix=MARKERLESS
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-support-indicator-bit -ptsan-may-decode-raw -ptsan-reoptimize=false %s | FileCheck %s --check-prefix=MIXED
; RUN: not --crash %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-may-decode-raw %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=INVALID
; INVALID: ptsan-may-decode-raw requires ptsan-support-indicator-bit
; RUN: not --crash %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode=0 -ptsan-support-indicator-bit %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=INVALID-SUPPORT
; INVALID-SUPPORT: ptsan-support-indicator-bit requires strict 32-bit IDs

target triple = "x86_64-unknown-linux-musl"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

; Both modes use mapped metadata; strict mode changes the pointer encoding.
; MARKERLESS: @ptsan_bounds = external global ptr
; MARKER: @ptsan_bounds = external global ptr
define i32 @read_pointer(ptr %p) {
  %value = load volatile i32, ptr %p
  ret i32 %value
}

; MARKERLESS-LABEL: define i32 @read_pointer(
; MARKERLESS: lshr i64 {{.*}}, 32
; MARKERLESS-NOT: and i64 {{.*}}, 2147483647
; MARKERLESS-NOT: icmp slt i64 {{.*}}, -1
; MARKERLESS: ret i32
; MARKER-LABEL: define i32 @read_pointer(
; MARKER: lshr i64 {{.*}}, 32
; MARKER-NOT: and i64 {{.*}}, 2147483647
; MARKER: icmp slt i64 {{.*}}, -1
; MARKER: select i1
; MARKER: ret i32
; MIXED-LABEL: define i32 @read_pointer(
; MIXED: lshr i64 {{.*}}, 32
; MIXED-NOT: and i64 {{.*}}, 2147483647
; MIXED: icmp slt i64 {{.*}}, -1
; MIXED: select i1
; MIXED: ret i32

define ptr @advance_pointer(ptr %p, i64 %n) {
  %next = getelementptr i8, ptr %p, i64 %n
  ret ptr %next
}

; Non-inbounds arithmetic decodes then repacks. Marked encoding preserves raw
; zero-ID pointers on that round trip; strict mode requires encoded inputs.
; MARKERLESS-LABEL: define ptr @advance_pointer(
; MARKERLESS-NOT: icmp eq i64 {{.*}}, 0
; MARKERLESS: ret ptr
; MARKER-LABEL: define ptr @advance_pointer(
; MARKER: icmp eq i64 {{.*}}, 0
; MARKER: select i1
; MARKER: ret ptr
; MIXED-LABEL: define ptr @advance_pointer(
; MIXED: icmp eq i64 {{.*}}, 0
; MIXED: select i1
; MIXED: ret ptr
