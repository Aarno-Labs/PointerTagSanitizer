; This regression test covers sibling CFG paths that access the same pointer.
; Strip anchors should be hoisted and merged, but bounds anchors must remain
; available at each check site so lowering never reuses non-dominating values.
;
; RUN: %opt_ptsan -ptsan-reoptimize -ptsan-runtime-validate -ptsan-abort-on-error -ptsan-stack-temporal-safety=0 -ptsan-stop-after=optimization %s | FileCheck %s --check-prefix=OPT
; RUN: %opt_ptsan -ptsan-reoptimize -ptsan-runtime-validate -ptsan-abort-on-error -ptsan-stack-temporal-safety=0 %s -o /dev/null

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @opaque(ptr, i32, ptr)

define i32 @sibling_check_reuses_non_dominating_scev(ptr %p, i32 %sel) {
entry:
  %call0 = tail call i32 @opaque(ptr null, i32 0, ptr null)
  switch i32 %sel, label %default [
    i32 0, label %case0
    i32 1, label %case1
  ]

default:
  %call1 = tail call i32 @opaque(ptr null, i32 0, ptr null)
  %pad = add i32 0, 0
  %v0 = load i32, ptr %p, align 4
  ret i32 %v0

case0:
  %v1 = load i32, ptr %p, align 4
  ret i32 %v1

case1:
  %v2 = load i32, ptr %p, align 4
  ret i32 %v2
}

; OPT-LABEL: define i32 @sibling_check_reuses_non_dominating_scev(
; OPT-LABEL: entry:
; OPT: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; OPT: [[STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; OPT-NOT: call ptr @__ptsan_strip(ptr %p)
; OPT: [[ENTRY_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[ID]])
; OPT: [[ENTRY_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; OPT: switch i32 %sel
; OPT-LABEL: default:
; OPT-NOT: call ptr @__ptsan_strip(ptr %p)
; OPT: [[DEFAULT_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[ID]])
; OPT: [[DEFAULT_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; OPT: call void (...) @__ptsan_access(i64 [[ID]], i64 [[DEFAULT_BASE]], i64 [[DEFAULT_SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 0)
; OPT: load i32, ptr [[STRIP]]
; OPT-LABEL: case0:
; OPT-NOT: call ptr @__ptsan_strip(ptr %p)
; OPT: call void (...) @__ptsan_access(i64 [[ID]], i64 [[ENTRY_BASE]], i64 [[ENTRY_SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 0)
; OPT: load i32, ptr [[STRIP]]
; OPT-LABEL: case1:
; OPT-NOT: call ptr @__ptsan_strip(ptr %p)
; OPT: call void (...) @__ptsan_access(i64 [[ID]], i64 [[ENTRY_BASE]], i64 [[ENTRY_SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 4, i32 1, i32 0)
; OPT: load i32, ptr [[STRIP]]
