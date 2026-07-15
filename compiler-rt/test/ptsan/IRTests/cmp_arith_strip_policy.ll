; Characterize the current cmp/arith strip policy before changing late strip-ID
; hoisting. Some of this is intentionally awkward: direct pointer compares are
; stripped during discovery even when -ptsan-clear-ids-on-cmp=0, while ptrtoint
; compares and pointer-subtraction arithmetic still obey the flags.
;
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=discovery -ptsan-clear-ids-on-cmp=0 -ptsan-clear-ids-on-arith=0 %s | FileCheck %s --check-prefix=DISCOVERY-NOCLEAR
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=discovery -ptsan-clear-ids-on-cmp=1 -ptsan-clear-ids-on-arith=1 %s | FileCheck %s --check-prefix=DISCOVERY-CLEAR
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization -ptsan-clear-ids-on-cmp=0 -ptsan-clear-ids-on-arith=0 %s | FileCheck %s --check-prefix=LATE-NOCLEAR

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i1 @direct_pointer_cmp(ptr %p, ptr %q) {
; DISCOVERY-NOCLEAR-LABEL: define i1 @direct_pointer_cmp(
; DISCOVERY-NOCLEAR-DAG: [[P:%.*]] = call ptr @__ptsan_strip(ptr %p)
; DISCOVERY-NOCLEAR-DAG: [[Q:%.*]] = call ptr @__ptsan_strip(ptr %q)
; DISCOVERY-NOCLEAR: icmp ult ptr [[P]], [[Q]]
;
; DISCOVERY-CLEAR-LABEL: define i1 @direct_pointer_cmp(
; DISCOVERY-CLEAR-DAG: [[P:%.*]] = call ptr @__ptsan_strip(ptr %p)
; DISCOVERY-CLEAR-DAG: [[Q:%.*]] = call ptr @__ptsan_strip(ptr %q)
; DISCOVERY-CLEAR: icmp ult ptr [[P]], [[Q]]
entry:
  %cmp = icmp ult ptr %p, %q
  ret i1 %cmp
}

define i1 @ptrtoint_cmp(ptr %p, ptr %q) {
; DISCOVERY-NOCLEAR-LABEL: define i1 @ptrtoint_cmp(
; DISCOVERY-NOCLEAR-NOT: call i64 @__ptsan_strip_i64
; DISCOVERY-NOCLEAR: icmp eq i64 %pi, %qi
;
; DISCOVERY-CLEAR-LABEL: define i1 @ptrtoint_cmp(
; DISCOVERY-CLEAR-DAG: [[PI:%.*]] = call i64 @__ptsan_strip_i64(i64 %pi)
; DISCOVERY-CLEAR-DAG: [[QI:%.*]] = call i64 @__ptsan_strip_i64(i64 %qi)
; DISCOVERY-CLEAR: icmp eq i64 [[PI]], [[QI]]
entry:
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %cmp = icmp eq i64 %pi, %qi
  ret i1 %cmp
}

define i64 @ptr_sub(ptr %p, ptr %q) {
; DISCOVERY-NOCLEAR-LABEL: define i64 @ptr_sub(
; DISCOVERY-NOCLEAR-NOT: call i64 @__ptsan_strip_i64
; DISCOVERY-NOCLEAR: sub i64 %pi, %qi
;
; DISCOVERY-CLEAR-LABEL: define i64 @ptr_sub(
; DISCOVERY-CLEAR-DAG: [[PI:%.*]] = call i64 @__ptsan_strip_i64(i64 %pi)
; DISCOVERY-CLEAR-DAG: [[QI:%.*]] = call i64 @__ptsan_strip_i64(i64 %qi)
; DISCOVERY-CLEAR: sub i64 [[PI]], [[QI]]
entry:
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %sub = sub i64 %pi, %qi
  ret i64 %sub
}

define i1 @late_no_clear_direct_cmp_can_hoist(ptr %base) {
; LATE-NOCLEAR-LABEL: define i1 @late_no_clear_direct_cmp_can_hoist(
; LATE-NOCLEAR: entry:
; LATE-NOCLEAR-NEXT: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %base), !ptsan.expected.stripped {{![0-9]+}}, !ptsan.stripped
; LATE-NOCLEAR: %lhs = getelementptr i8, ptr [[BASE_STRIP]], i64 8, !ptsan.expected.stripped {{![0-9]+}}, !ptsan.stripped
; LATE-NOCLEAR: %rhs = getelementptr i8, ptr [[BASE_STRIP]], i64 16, !ptsan.expected.stripped {{![0-9]+}}, !ptsan.stripped
; LATE-NOCLEAR: %cmp = icmp ult ptr %lhs, %rhs
; LATE-NOCLEAR-NOT: call ptr @__ptsan_strip(ptr %lhs)
; LATE-NOCLEAR-NOT: call ptr @__ptsan_strip(ptr %rhs)
entry:
  %lhs = getelementptr i8, ptr %base, i64 8
  %rhs = getelementptr i8, ptr %base, i64 16
  %cmp = icmp ult ptr %lhs, %rhs
  ret i1 %cmp
}
