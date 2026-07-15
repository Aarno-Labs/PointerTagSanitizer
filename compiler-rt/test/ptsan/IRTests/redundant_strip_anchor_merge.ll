; Discovery can create multiple strip anchors for repeated uses of the same
; value. Optimization should hoist and merge those anchors so each stripped
; value has one canonical strip anchor.
;
; RUN: %opt_ptsan -ptsan-stop-after=discovery %s | FileCheck %s --check-prefix=DISCOVERY
; RUN: %opt_ptsan -ptsan-stop-after=optimization %s | FileCheck %s --check-prefix=OPT

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i1 @redundant_ptr_strips(ptr %p) {
entry:
  %cmp = icmp eq ptr %p, %p
  ret i1 %cmp
}

define i64 @redundant_i64_strips(ptr %p) {
entry:
  %pi = ptrtoint ptr %p to i64
  %sub = sub i64 %pi, %pi
  ret i64 %sub
}

; DISCOVERY-LABEL: define i1 @redundant_ptr_strips(
; DISCOVERY: [[P0:%.*]] = call ptr @__ptsan_strip(ptr %p)
; DISCOVERY: [[P1:%.*]] = call ptr @__ptsan_strip(ptr %p)
; DISCOVERY: icmp eq ptr [[P0]], [[P1]]
; DISCOVERY-LABEL: define i64 @redundant_i64_strips(
; DISCOVERY: [[I0:%.*]] = call i64 @__ptsan_strip_i64(i64 %pi)
; DISCOVERY: [[I1:%.*]] = call i64 @__ptsan_strip_i64(i64 %pi)
; DISCOVERY: sub i64 [[I0]], [[I1]]

; OPT-LABEL: define i1 @redundant_ptr_strips(
; OPT: [[P:%.*]] = call ptr @__ptsan_strip(ptr %p)
; OPT-NOT: call ptr @__ptsan_strip(ptr %p)
; OPT: icmp eq ptr [[P]], [[P]]
; OPT-LABEL: define i64 @redundant_i64_strips(
; OPT: [[I:%.*]] = call i64 @__ptsan_strip_i64(i64 %pi)
; OPT-NOT: call i64 @__ptsan_strip_i64(i64 %pi)
; OPT: sub i64 [[I]], [[I]]
