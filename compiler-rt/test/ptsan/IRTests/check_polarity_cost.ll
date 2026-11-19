;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-no-opts -ptsan-stop-after=lowering %s | FileCheck %s --check-prefix=LOWER
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-no-opts -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=ID32-RAW
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-no-opts -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=ID32-TAGGED
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-support-indicator-bit -ptsan-no-opts -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=ID32-TAGGED
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -ptsan-support-indicator-bit -ptsan-may-decode-raw -ptsan-no-opts -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=ID32-RAW
;
; Check the cost model, not just whether either representation can be checked.
; One base mask is cheaper than a retag, but repeating that mask inside a loop
; is more expensive than tagging once outside it. Disable check hoisting to
; keep the loop check in the loop. Runtime coverage is in check_polarity.c.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; A dynamic alloca starts stripped. For one check, mask the stored base rather
; than construct a tagged pointer just for the subtraction.
define i8 @stripped_check(i64 %n) {
; LATE-LABEL: define i8 @stripped_check(
; LATE: [[RAW:%.*]] = call ptr @__ptsan_source(ptr %buf)
; LATE-NOT: call ptr @__ptsan_tag(
; LATE: call void (...) @__ptsan_access({{.*}}ptr [[RAW]],
; LATE: load volatile i8, ptr [[RAW]]
; LOWER-LABEL: define i8 @stripped_check(
; LOWER: [[RAW_BITS:%.*]] = ptrtoint ptr %buf to i64
; LOWER: [[BASE:%.*]] = load i64, ptr {{.*}}baseGep
; LOWER: [[RAW_BASE:%.*]] = and i64 [[BASE]], 281474976710655
; LOWER: sub i64 [[RAW_BITS]], [[RAW_BASE]]
entry:
  %buf = alloca i8, i64 %n
  %v = load volatile i8, ptr %buf
  ret i8 %v
}

; The same input should be tagged once when that avoids masking on every loop
; iteration. Memory accesses still use the raw pointer.
define void @loop_check(i64 %n, i1 %done) {
; LATE-LABEL: define void @loop_check(
; LATE: [[SOURCE:%.*]] = call ptr @__ptsan_source(ptr %buf)
; LATE: [[TAGGED:%.*]] = call ptr @__ptsan_tag(ptr [[SOURCE]],
; LATE: loop:
; LATE-NOT: call ptr @__ptsan_tag(
; LATE: call void (...) @__ptsan_access({{.*}}ptr [[TAGGED]],
; LATE: load volatile i8, ptr [[SOURCE]]
; LOWER-LABEL: define void @loop_check(
; LOWER: loop:
; LOWER: [[LOOP_BASE:%.*]] = load i64, ptr {{.*}}baseGep
; LOWER-NOT: and i64 [[LOOP_BASE]]
; LOWER: sub i64 {{.*}}, [[LOOP_BASE]]
entry:
  %buf = alloca i8, i64 %n
  br label %loop
loop:
  %v = load volatile i8, ptr %buf
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; An ID16 check can use a tagged argument without a conversion or base mask.
; With conditional ID32 decoding, the check should instead share the strip
; needed for the load.
; Unconditional ID32 decoding has equal check costs and does not need to
; change the check's incoming polarity.
define i8 @tagged_check(ptr %p) {
; LATE-LABEL: define i8 @tagged_check(
; LATE: call void (...) @__ptsan_access({{.*}}ptr %p,
; LOWER-LABEL: define i8 @tagged_check(
; LOWER: [[BITS:%.*]] = ptrtoint ptr %p to i64
; LOWER: [[TAGGED_BASE:%.*]] = load i64, ptr {{.*}}baseGep
; LOWER-NOT: and i64 [[TAGGED_BASE]]
; LOWER: sub i64 [[BITS]], [[TAGGED_BASE]]
; ID32-RAW-LABEL: define i8 @tagged_check(
; ID32-RAW: [[RAW:%.*]] = call ptr @__ptsan_strip_id32(ptr %p,
; ID32-RAW: call void (...) @__ptsan_access({{.*}}ptr [[RAW]],
; ID32-RAW: load volatile i8, ptr [[RAW]]
; ID32-TAGGED-LABEL: define i8 @tagged_check(
; ID32-TAGGED: call void (...) @__ptsan_access({{.*}}ptr %p,
entry:
  %v = load volatile i8, ptr %p
  ret i8 %v
}
