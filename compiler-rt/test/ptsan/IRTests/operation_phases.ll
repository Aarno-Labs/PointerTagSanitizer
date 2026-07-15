; This test exercises the internal PTSan phase-stop hooks. It checks that
; discovery emits compiler-only anchors, optimization normalizes them, metadata
; lowering materializes check range operands, late optimization can still leave
; anchor IR inspectable, and normal lowering removes every anchor before final
; IR is produced.
;
; RUN: %opt_ptsan -ptsan-stop-after=prepare %s | FileCheck %s --check-prefix=PREPARE
; RUN: %opt_ptsan -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DISCOVERY
; RUN: %opt_ptsan -ptsan-stop-after=optimization %s | FileCheck %s --check-prefix=OPT
; RUN: %opt_ptsan -ptsan-stop-after=metadata-lowering %s | FileCheck %s --check-prefix=META
; RUN: %opt_ptsan -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=LATE
; RUN: %opt_ptsan %s | FileCheck %s --check-prefix=LOWER

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @read_arg(ptr %p) {
entry:
  %v = load i8, ptr %p, align 1
  ret void
}

define i1 @same_operand_strip(ptr %p) {
entry:
  %cmp = icmp eq ptr %p, %p
  ret i1 %cmp
}

define i64 @ptrtoint_strip(ptr %p, ptr %q) {
entry:
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %sub = sub i64 %pi, %qi
  ret i64 %sub
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %buf = alloca [4 x i8], align 1
  call void @read_arg(ptr %buf)
  ret i32 0
}

; PREPARE-NOT: call void (...) @__ptsan_access
; PREPARE-NOT: call i64 @__ptsan_get_id
; PREPARE-NOT: call i64 @__ptsan_load_base
; PREPARE-NOT: call i64 @__ptsan_load_span
; PREPARE-NOT: call i64 @__ptsan_static_base
; PREPARE-NOT: call i64 @__ptsan_static_span
; PREPARE-NOT: call {{.*}} @__ptsan_strip
; PREPARE-NOT: call void (...) @__ptsan_source
; PREPARE-NOT: call void (...) @__ptsan_function_exit

; DISCOVERY-DAG: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; DISCOVERY-DAG: [[BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
; DISCOVERY-DAG: [[SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[ID]])
; DISCOVERY-DAG: call void (...) @__ptsan_access(i64 [[ID]], i64 [[BASE]], i64 [[SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 1, i32 1, i32 0)
; DISCOVERY-DAG: call ptr @__ptsan_strip(ptr %p)
; DISCOVERY-DAG: call ptr @__ptsan_strip(ptr %p)
; DISCOVERY-DAG: call i64 @__ptsan_strip_i64(i64 %pi)
; DISCOVERY-DAG: call i64 @__ptsan_strip_i64(i64 %qi)
; DISCOVERY-DAG: call void (...) @__ptsan_source
; DISCOVERY-DAG: call void (...) @__ptsan_function_exit
; DISCOVERY: declare void @__ptsan_access(...)
; DISCOVERY: declare i64 @__ptsan_get_id(ptr)
; DISCOVERY: declare i64 @__ptsan_load_base(i64)
; DISCOVERY: declare i64 @__ptsan_load_span(i64)
; DISCOVERY: declare ptr @__ptsan_strip(ptr)
; DISCOVERY: declare i64 @__ptsan_strip_i64(i64)

; OPT-DAG: [[OPT_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; OPT-DAG: [[OPT_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[OPT_ID]])
; OPT-DAG: [[OPT_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[OPT_ID]])
; OPT-DAG: call void (...) @__ptsan_access(i64 [[OPT_ID]], i64 [[OPT_BASE]], i64 [[OPT_SPAN]], ptr @__ptsan_unmaterialized_range, i64 0, i64 1, i32 1, i32 0)
; OPT-DAG: call ptr @__ptsan_strip(ptr %p)
; OPT-DAG: call i64 @__ptsan_strip_i64(i64 %pi)
; OPT-DAG: call i64 @__ptsan_strip_i64(i64 %qi)
; OPT-DAG: call void (...) @__ptsan_source
; OPT-DAG: call void (...) @__ptsan_function_exit
; OPT: declare void @__ptsan_access(...)
; OPT: declare i64 @__ptsan_get_id(ptr)
; OPT: declare i64 @__ptsan_load_base(i64)
; OPT: declare i64 @__ptsan_load_span(i64)
; OPT: declare ptr @__ptsan_strip(ptr)
; OPT: declare i64 @__ptsan_strip_i64(i64)

; META-DAG: [[META_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; META-DAG: [[META_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[META_ID]])
; META-DAG: [[META_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[META_ID]])
; META-DAG: call void (...) @__ptsan_access(i64 [[META_ID]], i64 [[META_BASE]], i64 [[META_SPAN]], ptr %p, i64 1, i64 1, i32 1, i32 0)
; META-DAG: call ptr @__ptsan_strip(ptr %p)
; META-DAG: call i64 @__ptsan_strip_i64(i64 %pi)
; META-DAG: call i64 @__ptsan_strip_i64(i64 %qi)
; META-DAG: call void (...) @__ptsan_source
; META-DAG: call void (...) @__ptsan_function_exit
; META: declare void @__ptsan_access(...)
; META: declare i64 @__ptsan_get_id(ptr)
; META: declare i64 @__ptsan_load_base(i64)
; META: declare i64 @__ptsan_load_span(i64)
; META: declare ptr @__ptsan_strip(ptr)
; META: declare i64 @__ptsan_strip_i64(i64)

; LATE-DAG: [[LATE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; LATE-DAG: [[LATE_BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[LATE_ID]])
; LATE-DAG: [[LATE_SPAN:%.*]] = call i64 @__ptsan_load_span(i64 [[LATE_ID]])
; LATE-DAG: [[LATE_MIN:%.*]] = call ptr @__ptsan_tagged(ptr %p)
; LATE-DAG: call void (...) @__ptsan_access(i64 [[LATE_ID]], i64 [[LATE_BASE]], i64 [[LATE_SPAN]], ptr [[LATE_MIN]], i64 1, i64 1, i32 1, i32 0)
; LATE-DAG: call ptr @__ptsan_strip(ptr %p)
; LATE-DAG: call i64 @__ptsan_strip_i64(i64 %pi)
; LATE-DAG: call i64 @__ptsan_strip_i64(i64 %qi)
; LATE-DAG: call void (...) @__ptsan_source
; LATE-DAG: call void (...) @__ptsan_function_exit
; LATE: declare void @__ptsan_access(...)
; LATE: declare i64 @__ptsan_get_id(ptr)
; LATE: declare i64 @__ptsan_load_base(i64)
; LATE: declare i64 @__ptsan_load_span(i64)
; LATE: declare ptr @__ptsan_strip(ptr)
; LATE: declare i64 @__ptsan_strip_i64(i64)

; LOWER-NOT: call void (...) @__ptsan_access
; LOWER-NOT: call i64 @__ptsan_get_id
; LOWER-NOT: call i64 @__ptsan_load_base
; LOWER-NOT: call i64 @__ptsan_load_span
; LOWER-NOT: call i64 @__ptsan_static_base
; LOWER-NOT: call i64 @__ptsan_static_span
; LOWER-NOT: call {{.*}} @__ptsan_strip
; LOWER-NOT: call void (...) @__ptsan_source
; LOWER-NOT: call void (...) @__ptsan_function_exit
; LOWER: call void @ptsan_complain
