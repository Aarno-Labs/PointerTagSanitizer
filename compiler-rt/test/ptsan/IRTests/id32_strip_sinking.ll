;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks %s | FileCheck %s --check-prefix=LOWER

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr, i64, i64) nofree
declare i64 @__ptsan_get_id(ptr) nofree
declare i64 @__ptsan_load_base(i64) nofree
declare void @may_free()
declare ptr @dlsym(ptr, ptr)

; Remove an unused conversion and its metadata dependencies before sinking.
define void @unused_strip(ptr %p) {
; CHECK-LABEL: define void @unused_strip(
; CHECK-NOT: @__ptsan_get_id
; CHECK-NOT: @__ptsan_load_base
; CHECK-NOT: @__ptsan_strip
; CHECK: ret void
entry:
  %id = call i64 @__ptsan_get_id(ptr %p)
  %base = call i64 @__ptsan_load_base(i64 %id)
  %raw = call ptr @__ptsan_strip(ptr %p, i64 %id, i64 %base)
  ret void
}

define i8 @sink_strip_below_null_guard(ptr %slot) {
; CHECK-LABEL: define i8 @sink_strip_below_null_guard(
; CHECK: entry:
; CHECK: [[SLOT_ID:%.*]] = call i64 @__ptsan_get_id(ptr %slot)
; CHECK: [[SLOT_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[SLOT_ID]])
; CHECK: [[SLOT:%.*]] = call ptr @__ptsan_strip(ptr %slot, i64 [[SLOT_ID]], i64 [[SLOT_BASE]])
; CHECK: [[P:%.*]] = load ptr, ptr [[SLOT]]
; CHECK-NEXT: [[P_ID:%.*]] = call i64 @__ptsan_get_id(ptr [[P]])
; CHECK-NEXT: [[NULL:%.*]] = icmp eq ptr [[P]], null
; CHECK-NEXT: br i1 [[NULL]], label %null, label %nonnull
; CHECK: nonnull:
; CHECK-NEXT: [[P_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[P_ID]])
; CHECK-NEXT: [[STRIPPED:%.*]] = call ptr @__ptsan_strip(ptr [[P]], i64 [[P_ID]], i64 [[P_BASE]])
; CHECK-NEXT: [[V:%.*]] = load i8, ptr [[STRIPPED]]
entry:
  %slot.id = call i64 @__ptsan_get_id(ptr %slot)
  %slot.base = call i64 @__ptsan_load_base(i64 %slot.id)
  %slot.stripped = call ptr @__ptsan_strip(ptr %slot, i64 %slot.id, i64 %slot.base)
  %p = load ptr, ptr %slot.stripped
  %p.id = call i64 @__ptsan_get_id(ptr %p)
  %p.base = call i64 @__ptsan_load_base(i64 %p.id)
  %p.stripped = call ptr @__ptsan_strip(ptr %p, i64 %p.id, i64 %p.base)
  %isnull = icmp eq ptr %p, null
  br i1 %isnull, label %null, label %nonnull

nonnull:
  %v = load i8, ptr %p.stripped
  ret i8 %v

null:
  ret i8 0
}

; Sinking within a block must keep the decode base before the strip, even when
; both move past a freeing call after the PHI.
define i8 @phi_before_freeing_call(ptr %p, ptr %q, i1 %choose) {
; CHECK-LABEL: define i8 @phi_before_freeing_call(
; CHECK: join:
; CHECK: [[PTR:%.*]] = phi ptr
; CHECK: [[PHI_ID:%.*]] = call i64 @__ptsan_get_id(ptr [[PTR]])
; CHECK: call void @may_free()
; CHECK: [[PHI_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[PHI_ID]])
; CHECK: [[PHI_RAW:%.*]] = call ptr @__ptsan_strip(ptr [[PTR]], i64 [[PHI_ID]], i64 [[PHI_BASE]])
; CHECK: load i8, ptr [[PHI_RAW]]
entry:
  br i1 %choose, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %pointer = phi ptr [ %p, %left ], [ %q, %right ]
  %id = call i64 @__ptsan_get_id(ptr %pointer)
  %base = call i64 @__ptsan_load_base(i64 %id)
  %raw = call ptr @__ptsan_strip(ptr %pointer, i64 %id, i64 %base)
  call void @may_free()
  %value = load i8, ptr %raw
  ret i8 %value
}


; A use inside a loop should still allow sinking to the conditional preheader,
; rather than keeping the metadata load on the path that skips the loop.
define i64 @sink_to_loop_preheader(ptr %p, i1 %enter, i64 %count) {
; CHECK-LABEL: define i64 @sink_to_loop_preheader(
; CHECK: entry:
; CHECK: [[LOOP_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; CHECK-NOT: call i64 @__ptsan_load_base
; CHECK: br i1 %enter
; CHECK: preheader:
; CHECK: [[LOOP_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[LOOP_ID]])
; CHECK: br label %loop
; CHECK: loop:
; CHECK-NOT: call i64 @__ptsan_load_base
; CHECK: add i64 [[LOOP_BASE]],
entry:
  %id = call i64 @__ptsan_get_id(ptr %p)
  %base = call i64 @__ptsan_load_base(i64 %id)
  br i1 %enter, label %preheader, label %skip
preheader:
  br label %loop
loop:
  %i = phi i64 [ 0, %preheader ], [ %next, %loop ]
  %value = add i64 %base, %i
  %next = add i64 %i, 1
  %again = icmp ult i64 %next, %count
  br i1 %again, label %loop, label %done
done:
  ret i64 %value
skip:
  ret i64 0
}

; A PHI consumes its incoming value on the predecessor edge, not in the join.
define i64 @sink_to_phi_edge(ptr %p, i1 %choose) {
; CHECK-LABEL: define i64 @sink_to_phi_edge(
; CHECK: entry:
; CHECK: [[EDGE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; CHECK-NOT: call i64 @__ptsan_load_base
; CHECK: br i1 %choose
; CHECK: left:
; CHECK: [[EDGE_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[EDGE_ID]])
; CHECK: br label %join
; CHECK: join:
; CHECK: phi i64 [ [[EDGE_BASE]], %left ], [ 0, %right ]
entry:
  %id = call i64 @__ptsan_get_id(ptr %p)
  %base = call i64 @__ptsan_load_base(i64 %id)
  br i1 %choose, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %value = phi i64 [ %base, %left ], [ 0, %right ]
  ret i64 %value
}

; Sinking may place a strip between a positional marker and its call. Lowering
; must still bracket dlsym, not the instructions emitted for the strip.
define ptr @sink_strip_past_libc_marker(ptr %name, i1 %lookup) {
; CHECK-LABEL: define ptr @sink_strip_past_libc_marker(
; CHECK: find:
; CHECK: @__ptsan_libc_call(
; CHECK: [[NAME:%.*]] = call ptr @__ptsan_strip(ptr %name,
; CHECK: call ptr @dlsym(ptr {{.*}}, ptr [[NAME]])
; LOWER-LABEL: define ptr @sink_strip_past_libc_marker(
; LOWER: find:
; LOWER: [[OLD:%.*]] = load i32, ptr @ptsan_in_libc
; LOWER: store i32 {{.*}}, ptr @ptsan_in_libc
; LOWER: call ptr @dlsym(
; LOWER: store i32 [[OLD]], ptr @ptsan_in_libc
entry:
  %id = call i64 @__ptsan_get_id(ptr %name)
  %base = call i64 @__ptsan_load_base(i64 %id)
  %raw = call ptr @__ptsan_strip(ptr %name, i64 %id, i64 %base)
  br i1 %lookup, label %find, label %skip
find:
  %result = call ptr @dlsym(ptr null, ptr %raw)
  ret ptr %result
skip:
  ret ptr null
}
