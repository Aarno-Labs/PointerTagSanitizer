;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=late-optimization -verify-each %s | FileCheck %s --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-strict-mode -ptsan-runtime-validate -verify-each %s -o /dev/null
; RUN: %opt_ptsan -ptsan-no-opts -verify-each %s -o /dev/null
; RUN: %opt_ptsan -ptsan-32-bit-ids -verify-each %s -o /dev/null
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=late-optimization -verify-each %s | FileCheck %s --check-prefix=ID32
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-strict-mode -verify-each %s -o /dev/null
; RUN: opt -passes=ptsan-id-strip -verify-each -S %s | FileCheck %s --check-prefix=STRIP
; RUN: opt -passes=ptsan-id-strip -ptsan-32-bit-ids -verify-each -S %s -o /dev/null

; A callbr result is available on all successors, with no single insertion
; point after its definition. Test conversions on both successors, after a
; join, on duplicate edges, and for an existing PHI on a critical edge.
target triple = "x86_64-unknown-linux-gnu"

define i32 @both_successors(i64 %bits) {
; LATE-LABEL: define i32 @both_successors(
; LATE: %p = callbr ptr
; LATE: fallthrough:
; LATE: call ptr @__ptsan_strip(ptr %p)
; LATE: taken:
; LATE: call ptr @__ptsan_strip(ptr %p)
; ID32-LABEL: define i32 @both_successors(
; ID32: %p = callbr ptr
; ID32: fallthrough:
; ID32: [[FID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; ID32: call ptr @__ptsan_strip_id32(ptr %p, i64 [[FID]]
; ID32: taken:
; ID32: [[TID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; ID32: call ptr @__ptsan_strip_id32(ptr %p, i64 [[TID]]
; STRIP-LABEL: define i32 @both_successors(
; STRIP: fallthrough:
; STRIP-NEXT: [[F:%.*]] = ptrtoint ptr %p to i64
; STRIP-NEXT: [[FB:%.*]] = call i64 asm {{.*}}(i64 [[F]], i64 281474976710655)
; STRIP-NEXT: [[FS:%.*]] = inttoptr i64 [[FB]] to ptr
; STRIP-NEXT: %a = load i32, ptr [[FS]]
; STRIP: taken:
; STRIP-NEXT: [[T:%.*]] = ptrtoint ptr %p to i64
; STRIP-NEXT: [[TB:%.*]] = call i64 asm {{.*}}(i64 [[T]], i64 281474976710655)
; STRIP-NEXT: [[TS:%.*]] = inttoptr i64 [[TB]] to ptr
; STRIP-NEXT: %b = load i32, ptr [[TS]]
entry:
  %p = callbr ptr asm "mov $1, $0", "=r,r,!i"(i64 %bits)
          to label %fallthrough [label %taken]
fallthrough:
  %a = load i32, ptr %p
  ret i32 %a
taken:
  %b = load i32, ptr %p
  ret i32 %b
}

define i32 @join(i64 %bits) {
; Neither successor dominates the join: place the conversion at the join.
; ID32-LABEL: define i32 @join(
; ID32: join:
; ID32: [[JID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; ID32: call ptr @__ptsan_strip_id32(ptr %p, i64 [[JID]]
entry:
  %p = callbr ptr asm "mov $1, $0", "=r,r,!i"(i64 %bits)
          to label %fallthrough [label %taken]
fallthrough:
  br label %join
taken:
  br label %join
join:
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @duplicate_successor(i64 %bits) {
; Duplicate edges still have one usable destination block.
; ID32-LABEL: define i32 @duplicate_successor(
; ID32: use:
; ID32: call i64 @__ptsan_get_id(ptr %p)
; ID32: call ptr @__ptsan_strip_id32(ptr %p,
entry:
  %p = callbr ptr asm "mov $1, $0", "=r,r,!i"(i64 %bits)
          to label %use [label %use]
use:
  %v = load i32, ptr %p
  ret i32 %v
}

define ptr @phi_and_loop(ptr %other, i64 %bits, i1 %choose) {
; LATE-LABEL: define ptr @phi_and_loop(
; LATE: join:
; LATE-NEXT: %p = phi ptr [ %from.asm, %asm ], [ %other, %entry ], !ptsan.tagged
entry:
  br i1 %choose, label %asm, label %join
asm:
  %from.asm = callbr ptr asm "mov $1, $0", "=r,r,!i"(i64 %bits)
          to label %join [label %exit]
join:
  %p = phi ptr [ %from.asm, %asm ], [ %other, %entry ]
  br label %loop
loop:
  %i = phi i64 [ 0, %join ], [ %next, %loop ]
  %g = getelementptr i8, ptr %p, i64 %i
  %v = load volatile i8, ptr %g
  %next = add i64 %i, 1
  %finished = icmp eq i64 %next, 4
  br i1 %finished, label %done, label %loop
done:
  ret ptr %g
exit:
  ret ptr %from.asm
}

; Non-pointer results also need a legal insertion point when used as the mask
; for a vector bounds load.
declare <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr>, i32 immarg, <2 x i1>, <2 x i32>)
define <2 x i32> @mask_result(<2 x ptr> %ptrs) {
entry:
  %mask = callbr <2 x i1> asm "", "=r,!i"()
          to label %fallthrough [label %taken]
fallthrough:
  %v = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %ptrs, i32 4, <2 x i1> %mask, <2 x i32> zeroinitializer)
  ret <2 x i32> %v
taken:
  ret <2 x i32> zeroinitializer
}

; Pointer-vector results need the same block-local ID/strip placement.
define <2 x i32> @vector_result() {
entry:
  %ptrs = callbr <2 x ptr> asm "", "=x,!i"()
          to label %fallthrough [label %taken]
fallthrough:
  %v = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %ptrs, i32 4, <2 x i1> <i1 true, i1 true>, <2 x i32> zeroinitializer)
  ret <2 x i32> %v
taken:
  %w = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(<2 x ptr> %ptrs, i32 4, <2 x i1> <i1 true, i1 true>, <2 x i32> zeroinitializer)
  ret <2 x i32> %w
}

; Loop checks may recover the original callbr through SCEV. Conversion
; placement must work for that value directly, without synthetic PHIs.
define void @loop_output(i64 %bits) {
entry:
  %p = callbr ptr asm "mov $1, $0", "=r,r,!i"(i64 %bits)
          to label %loop [label %exit]
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %p, i64 %i
  %v = load volatile i8, ptr %g
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
