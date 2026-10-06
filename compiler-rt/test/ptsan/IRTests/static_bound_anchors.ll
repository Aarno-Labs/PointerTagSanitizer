; This test checks the static-bound rewrite for anchor-backed checks. A fixed
; alloca should keep the explicit get-id dependency, but its runtime load-bound
; anchors should become scalar static-base/static-span anchors.
;
; RUN: %opt_ptsan -ptsan-stop-after=optimization -ptsan-no-filter-removable-checks %s | FileCheck %s --check-prefix=OPT

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i8 @static_alloca() {
entry:
  %buf = alloca [4 x i8], align 1
  %v = load i8, ptr %buf, align 1
  ret i8 %v
}

declare void @possibly_frees_memory()

define i8 @static_alloca_across_call() {
entry:
  %buf = alloca [4 x i8], align 1
  call void @possibly_frees_memory()
  %v = load i8, ptr %buf, align 1
  ret i8 %v
}

; OPT-LABEL: define i8 @static_alloca(
; OPT-DAG: [[SOURCE:%.*]] = call ptr @__ptsan_source(ptr %buf)
; OPT-DAG: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr [[SOURCE]])
; OPT-DAG: [[SPAN:%.*]] = call i64 @__ptsan_static_span(i64 4)
; OPT-DAG: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr [[SOURCE]])
; OPT-DAG: [[BASE:%.*]] = call i64 @__ptsan_static_base(ptr [[TAGGED]])
; OPT: call void (...) @__ptsan_access(i64 [[ID]], i64 [[BASE]], i64 [[SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 1, i32 1, i32 0)

; An alloca-backed SourceAnchor remains unfreeable. Static bounds may therefore
; be hoisted above a call which could free heap storage.
; OPT-LABEL: define i8 @static_alloca_across_call(
; OPT: [[SOURCE:%.*]] = call ptr @__ptsan_source(ptr %buf)
; OPT: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr [[SOURCE]])
; OPT: [[SPAN:%.*]] = call i64 @__ptsan_static_span(i64 4)
; OPT: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr [[SOURCE]])
; OPT: [[BASE:%.*]] = call i64 @__ptsan_static_base(ptr [[TAGGED]])
; OPT: invoke void @possibly_frees_memory()
; OPT: call void (...) @__ptsan_access(i64 [[ID]], i64 [[BASE]], i64 [[SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 1, i32 1, i32 0)
