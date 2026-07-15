; Regression test for materialized check ranges whose minimum pointer is null.
; The placeholder for an unmaterialized range is also rooted at null, so
; lowering must distinguish "unmaterialized" from a real null range with a
; nonzero checked size.
;
; RUN: %opt_ptsan -ptsan-stop-after=metadata-lowering -ptsan-no-filter-removable-checks %s | FileCheck %s --check-prefix=META
; RUN: %opt_ptsan -ptsan-no-filter-removable-checks %s | FileCheck %s --check-prefix=LOWER

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i8 @load_null() {
entry:
  %v = load i8, ptr null, align 1
  ret i8 %v
}

; META-LABEL: define i8 @load_null(
; META-DAG: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr null)
; META-DAG: [[BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; META-DAG: [[SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[ID]])
; META: call void (...) @__ptsan_access(i64 [[ID]], i64 [[BASE]], i64 [[SPAN]], ptr null, i64 1, i64 1, i32 1, i32 0)

; LOWER-LABEL: define i8 @load_null(
; LOWER-NOT: call void (...) @__ptsan_access
; LOWER: call void @ptsan_complain
